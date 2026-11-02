// SPDX-License-Identifier: GPL-2.0
/*
 * ufs_mtk_config_gear_lane() - gear/lane power mode change for MT6991
 * (MTK vendor kernel 6.6, kernel_device_modules).
 *
 * Where: drivers/ufs/ufs-mediatek.c, directly after ufs_mtk_config_pwr_mode().
 *        It uses that file's static helpers.
 * Also:  add to drivers/ufs/ufs-mediatek.h, next to
 *        ufs_mtk_dynamic_clock_scaling():
 *
 *            int ufs_mtk_config_gear_lane(struct ufs_hba *hba, u32 gear, u32 lane);
 *
 * Same sequence as MTK's ufs_mtk_config_pwr_mode() (01_mtk_reference.c),
 * except:
 *  - takes hba->host_sem, so the error handler and other UFS sysfs writers
 *    can't run in the middle (clkscale_control does not take it: don't
 *    drive both at once)
 *  - forces FAST_MODE: anything else fails MTK's negotiation in
 *    ufs_mtk_pre_pwr_change(), which then sends an all-zero PMC
 *  - lowers the ufs_sel clock only after a successful PMC (MTK lowers it
 *    even on failure, which can leave the link at G5 on 273 MHz)
 *  - returns an error, and turns "PMC returned 0 but a host reset is
 *    queued" (AH8 disable failed in PRE_CHANGE) into -EIO
 *
 * Not compiled. Names checked against the public MT6991 vendor copy and
 * the android15-6.6 GKI headers.
 */

/* ---8<--- paste below this line --- */
int ufs_mtk_config_gear_lane(struct ufs_hba *hba, u32 gear, u32 lane)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	struct ufs_pa_layer_attr pwr_info;
	ktime_t timeout, time_checked;
	bool scale_allow, to_g5;
	int err;

	if (gear < UFS_HS_G1 || gear > hba->max_pwr_info.info.gear_rx ||
	    lane < 1 || lane > hba->max_pwr_info.info.lane_rx)
		return -EINVAL;

	/* same condition ufs_mtk_dynamic_clock_scaling() uses */
	scale_allow = hba->dev_info.wspecversion >= 0x0400 &&
		      ((host->caps & UFS_MTK_CAP_CLK_SCALE_ONLY_BY_USER) ||
		       hba->clk_scaling.is_initialized);
	to_g5 = gear >= UFS_HS_G5;

	memcpy(&pwr_info, &hba->pwr_info, sizeof(pwr_info));
	pwr_info.gear_rx = pwr_info.gear_tx = gear;
	pwr_info.lane_rx = pwr_info.lane_tx = lane;
	/* anything else makes MTK's negotiation send an all-zero PMC */
	pwr_info.pwr_rx = pwr_info.pwr_tx = FAST_MODE;

	/* serialize with the error handler and other UFS sysfs writers */
	down(&hba->host_sem);
	if (!ufshcd_is_user_access_allowed(hba)) {
		err = -EBUSY;
		goto unlock;
	}

	__pm_stay_awake(host->ufs_wake_lock);

	/* make sure pm resume done */
	timeout = ktime_add_ms(ktime_get(), UFS_WAKE_LOCK_TIMEOUT_MS);
	do {
		time_checked = ktime_get();

		/* Wait until suspend flow exit */
		if (pm_suspend_target_state == PM_SUSPEND_ON)
			goto rpm;

		usleep_range(100, 200);
	} while (ktime_before(time_checked, timeout));

	dev_warn(hba->dev, "Wait suspend status timeout, status = %d\n",
		 pm_suspend_target_state);
	err = -EBUSY;
	goto relax;
rpm:
	/* resume the device W-LUN: link out of hibern8, UniPro up, AHIT back */
	ufshcd_rpm_get_sync(hba);
	ufs_mtk_scsi_block_requests(hba);
	/* no clock gating from here on */
	ufshcd_hold(hba);

	err = ufs_mtk_wait_for_doorbell_clr(hba, 1000 * 1000); /* 1 sec */
	if (err) {
		dev_err(hba->dev, "%s: ufshcd_wait_for_doorbell_clr timeout!\n",
			__func__);
		goto out;
	}

	/* G5 needs ufs_sel at max (and vcore up) before the PMC */
	if (to_g5 && scale_allow) {
		ufs_mtk_pm_qos(hba, true);
		_ufs_mtk_clk_scale(hba, true);
	}

	err = ufshcd_config_pwr_mode(hba, &pwr_info);

	/* AH8 disable failed in PRE_CHANGE: a host reset is already queued */
	if (!err && hba->ufshcd_state != UFSHCD_STATE_OPERATIONAL)
		err = -EIO;

	/* lower the clock only once the link is really below G5 */
	if (!err && !to_g5 && scale_allow) {
		ufs_mtk_pm_qos(hba, false);
		_ufs_mtk_clk_scale(hba, false);
	}

	dev_info(hba->dev, "%s: err=%d [RX, TX]: gear=[%d, %d], lane=[%d, %d]\n",
		 __func__, err, hba->pwr_info.gear_rx, hba->pwr_info.gear_tx,
		 hba->pwr_info.lane_rx, hba->pwr_info.lane_tx);
out:
	ufshcd_release(hba);
	ufs_mtk_scsi_unblock_requests(hba);
	ufshcd_rpm_put(hba);
relax:
	__pm_relax(host->ufs_wake_lock);
unlock:
	up(&hba->host_sem);
	return err;
}
