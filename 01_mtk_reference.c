// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek's own power-mode-change wrapper, for comparison with the ported
 * debugfs code.
 *
 * Verbatim from MTK vendor kernel_device_modules-6.6/drivers/ufs/ufs-mediatek.c
 * lines 2188-2449, from a public MT6991 vendor kernel release.
 * Only the "[n]" comments are added. Full file: vendor_6.6/ufs-mediatek.c
 *
 * clkscale_control_store() -> ufs_mtk_dynamic_clock_scaling()
 *                          -> ufs_mtk_config_pwr_mode()
 *
 * [1] wake lock + wait: don't start while system suspend is in flight
 * [2] resume the device W-LUN: link out of hibern8 (DME_HIBERNATE_EXIT),
 *     UniPro powered up, chip back to Active, AHIT restored.
 *     pm_runtime_get_sync(hba->dev) does NOT do this since v5.14.
 * [3] stop new I/O
 * [4] keep clocks and ref-clk on (no clock gating)
 * [5] drain in-flight commands
 * [6] raise ufs_sel to 499.2 MHz (+ vcore >= 0.65 V) BEFORE going to G5
 * [7] the PMC: vops PRE_CHANGE (AH8 disable + wait for link UP)
 *     -> MTK pre-steps -> DME sets -> PA_PWRMODE -> POST_CHANGE
 * [8] lower ufs_sel AFTER leaving G5
 */

static void ufs_mtk_scsi_unblock_requests(struct ufs_hba *hba)
{
	if (atomic_dec_and_test(&hba->scsi_block_reqs_cnt))
		scsi_unblock_requests(hba->host);
}

static void ufs_mtk_scsi_block_requests(struct ufs_hba *hba)
{
	if (atomic_inc_return(&hba->scsi_block_reqs_cnt) == 1)
		scsi_block_requests(hba->host);
}

static u32 ufs_mtk_pending_cmds(struct ufs_hba *hba)
{
	const struct scsi_device *sdev;
	u32 pending = 0;

	lockdep_assert_held(hba->host->host_lock);
	__shost_for_each_device(sdev, hba->host)
		pending += sbitmap_weight(&sdev->budget_map);

	return pending;
}

static int ufs_mtk_wait_for_doorbell_clr(struct ufs_hba *hba,
					u64 wait_timeout_us)
{
	unsigned long flags;
	int ret = 0;
	u32 tm_doorbell;
	u32 tr_pending;
	bool timeout = false, do_last_check = false;
	ktime_t start;

	ufshcd_hold(hba);
	spin_lock_irqsave(hba->host->host_lock, flags);
	/*
	 * Wait for all the outstanding tasks/transfer requests.
	 * Verify by checking the doorbell registers are clear.
	 */
	start = ktime_get();
	do {
		if (hba->ufshcd_state != UFSHCD_STATE_OPERATIONAL) {
			ret = -EBUSY;
			goto out;
		}

		tm_doorbell = ufshcd_readl(hba, REG_UTP_TASK_REQ_DOOR_BELL);
		tr_pending = ufs_mtk_pending_cmds(hba);
		if (!tm_doorbell && !tr_pending) {
			timeout = false;
			break;
		} else if (do_last_check) {
			break;
		}

		spin_unlock_irqrestore(hba->host->host_lock, flags);
		io_schedule_timeout(msecs_to_jiffies(20));
		if (ktime_to_us(ktime_sub(ktime_get(), start)) >
		    wait_timeout_us) {
			timeout = true;
			/*
			 * We might have scheduled out for long time so make
			 * sure to check if doorbells are cleared by this time
			 * or not.
			 */
			do_last_check = true;
		}
		spin_lock_irqsave(hba->host->host_lock, flags);
	} while (tm_doorbell || tr_pending);

	if (timeout) {
		dev_err(hba->dev,
			"%s: timedout waiting for doorbell to clear (tm=0x%x, tr=0x%x)\n",
			__func__, tm_doorbell, tr_pending);
		ret = -EBUSY;
	}
out:
	spin_unlock_irqrestore(hba->host->host_lock, flags);
	ufshcd_release(hba);
	return ret;
}

static void ufs_mtk_config_pwr_mode(struct ufs_hba *hba, int mode,
	bool scale_allow)
{
	struct ufs_pa_layer_attr pwr_info = { 0 };
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	ktime_t timeout, time_checked;
	int err;

	/* Config desired power info */
	memcpy(&pwr_info, &hba->pwr_info, sizeof(struct ufs_pa_layer_attr));

	if (mode == CLK_FORCE_SCALE_UP) {
		pwr_info.gear_rx = host->max_gear;
		pwr_info.gear_tx = host->max_gear;
	} else if (mode == CLK_FORCE_SCALE_DOWN) {
		if (scale_allow) {
			/* min_gear is set by MTK */
			pwr_info.gear_rx = hba->clk_scaling.min_gear;
			pwr_info.gear_tx = hba->clk_scaling.min_gear;
		} else {
			/*
			 * min_gear is set by default, which is G1.
			 * Just set to max_gear because in this case, we only
			 * use max gear as default gear.
			 */
			pwr_info.gear_rx = host->max_gear;
			pwr_info.gear_tx = host->max_gear;
		}
	} else if (mode == CLK_FORCE_SCALE_G1) {
		pwr_info.gear_rx = UFS_HS_G1;
		pwr_info.gear_tx = UFS_HS_G1;
	}

	__pm_stay_awake(host->ufs_wake_lock);	/* [1] */

	/* make sure pm resume done */
	timeout = ktime_add_ms(ktime_get(), UFS_WAKE_LOCK_TIMEOUT_MS);
	do {
		time_checked = ktime_get();

		/* Wait until suspend flow exit */
		if (pm_suspend_target_state == PM_SUSPEND_ON)
			goto rpm;

		usleep_range(100, 200);
	} while (ktime_before(time_checked, timeout));

	dev_warn(hba->dev, "Wait suspend status timeout, status = %d\n", pm_suspend_target_state);
	__pm_relax(host->ufs_wake_lock);
	return;
rpm:
	ufshcd_rpm_get_sync(hba);	/* [2] */
	ufs_mtk_scsi_block_requests(hba);	/* [3] */
	ufshcd_hold(hba);	/* [4] */

	if (ufs_mtk_wait_for_doorbell_clr(hba, 1000 * 1000)) { /* 1 sec */	/* [5] */
		dev_err(hba->dev, "%s: ufshcd_wait_for_doorbell_clr timeout!\n",
				__func__);
		goto out;
	}

	/* Scale up, change clk mux before change gear */	/* [6] */
	if ((mode == CLK_FORCE_SCALE_UP) && scale_allow) {
		ufs_mtk_pm_qos(hba, true);
		_ufs_mtk_clk_scale(hba, true);
	}

	/* Start change power mode */
	err = ufshcd_config_pwr_mode(hba, &pwr_info);	/* [7] */
	if (err) {
		dev_err(hba->dev, "%s: Failed setting power mode, err = %d\n",
				__func__, err);
	} else {
		dev_info(hba->dev,
		      "%s: PMC Success! [RX, TX]: gear=[%d, %d], pwr[%d, %d]\n",
		      __func__,
		      hba->pwr_info.gear_rx, hba->pwr_info.gear_tx,
		      hba->pwr_info.pwr_rx, hba->pwr_info.pwr_tx);
	}

	/* Scale down, change clk mux after change gear */	/* [8] */
	if ((mode != CLK_FORCE_SCALE_UP) && scale_allow) {
		ufs_mtk_pm_qos(hba, false);
		_ufs_mtk_clk_scale(hba, false);
	}

out:
	ufshcd_release(hba);
	ufs_mtk_scsi_unblock_requests(hba);
	ufshcd_rpm_put(hba);
	__pm_relax(host->ufs_wake_lock);
}

void ufs_mtk_dynamic_clock_scaling(struct ufs_hba *hba, int mode)
{
	struct ufs_mtk_host *host = ufshcd_get_variant(hba);
	static int scale_mode = CLK_SCALE_FREE_RUN;
	static u32 saved_gear;
	static bool is_forced;
	unsigned long flags;
	bool scale_allow = true;
	bool scale_suspend = false;
	bool scale_resume = false;

	/* Not allow change clock scaling */
	if (host->clk_scale_forbid)
		return;

	/* Already in desire mode */
	if (scale_mode == mode)
		return;
	scale_mode = mode;

	/* UFS version is below 4.0, clock scaling is not necessary */
	if (hba->dev_info.wspecversion < 0x0400)
		scale_allow = false;

	/* Host not support clock scaling or disable by customer */
	if (!(host->caps & UFS_MTK_CAP_CLK_SCALE_ONLY_BY_USER) &&
	    !hba->clk_scaling.is_initialized)
		scale_allow = false;

	/*
	 * Make sure call devfreq_resume_device/devfreq_suspend_device not in
	 * UFS runtime suspend state. Or we may have abnormal scale up in
	 * runtime suspend which mtcmos is not on.
	 */
	ufshcd_rpm_get_sync(hba);

	if (mode == CLK_SCALE_FREE_RUN) {
		ufs_mtk_scsi_block_requests(hba);
		if (saved_gear >= UFS_HS_G5)
			ufs_mtk_config_pwr_mode(hba, CLK_FORCE_SCALE_UP,
				scale_allow);
		else
			ufs_mtk_config_pwr_mode(hba, CLK_FORCE_SCALE_DOWN,
				scale_allow);

		if (scale_allow) {
			spin_lock_irqsave(hba->host->host_lock, flags);
			if (hba->clk_scaling.is_suspended) {
				scale_resume = true;
				hba->clk_scaling.is_suspended = false;
			}
			spin_unlock_irqrestore(hba->host->host_lock, flags);

			if (scale_resume)
				devfreq_resume_device(hba->devfreq);
			hba->caps |= UFSHCD_CAP_CLK_SCALING;
			hba->clk_scaling.active_reqs = 0;
		}
		ufs_mtk_scsi_unblock_requests(hba);

		is_forced = false;
	} else {
		if (!is_forced) {
			if (scale_allow) {
				hba->caps &= ~UFSHCD_CAP_CLK_SCALING;
				spin_lock_irqsave(hba->host->host_lock, flags);
				if (!hba->clk_scaling.is_suspended) {
					scale_suspend = true;
					hba->clk_scaling.is_suspended = true;
					hba->clk_scaling.window_start_t = 0;
				}
				spin_unlock_irqrestore(hba->host->host_lock, flags);

				if (scale_suspend)
					devfreq_suspend_device(hba->devfreq);
			}

			saved_gear = hba->pwr_info.gear_rx;
			is_forced = true;
		}

		ufs_mtk_config_pwr_mode(hba, mode, scale_allow);
	}

	ufshcd_rpm_put(hba);
}
