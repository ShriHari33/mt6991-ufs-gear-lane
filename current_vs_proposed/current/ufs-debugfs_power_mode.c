// SPDX-License-Identifier: GPL-2.0
/*
 * CURRENT: the power_mode code that runs on the board today, in
 * kernel-6.6/drivers/ufs/core/ufs-debugfs.c.
 *
 * ufsdbg_config_pwr_mode() is the ported code as of 31 October, with
 * the CHANGED lines applied (file 06). ufsdbg_power_mode_validate() is the
 * G1-G5 version from file 06. ufsdbg_power_mode_write() is Qualcomm's
 * msm-4.19 handler as ported. ufsdbg_cfg_pwr_param() is left out on
 * both sides: the proposal doesn't change it.
 */

static bool ufsdbg_power_mode_validate(struct ufs_pa_layer_attr *pwr_mode)
{
	if (pwr_mode->gear_rx < UFS_HS_G1 || pwr_mode->gear_rx > UFS_HS_G5 ||
	    pwr_mode->gear_tx < UFS_HS_G1 || pwr_mode->gear_tx > UFS_HS_G5 ||
	    pwr_mode->lane_rx < 1 || pwr_mode->lane_rx > 2 ||
	    pwr_mode->lane_tx < 1 || pwr_mode->lane_tx > 2 ||
	    (pwr_mode->pwr_rx != FAST_MODE && pwr_mode->pwr_rx != FASTAUTO_MODE &&
	     pwr_mode->pwr_rx != SLOW_MODE && pwr_mode->pwr_rx != SLOWAUTO_MODE) ||
	    (pwr_mode->pwr_tx != FAST_MODE && pwr_mode->pwr_tx != FASTAUTO_MODE &&
	     pwr_mode->pwr_tx != SLOW_MODE && pwr_mode->pwr_tx != SLOWAUTO_MODE)) {
		pr_err("%s: power parameters are not valid\n", __func__);
		return false;
	}

	return true;
}

static int ufsdbg_config_pwr_mode(struct ufs_hba *hba,
		struct ufs_pa_layer_attr *desired_pwr_mode)
{
#define UFS_MIN_GEAR_TO_SCALE_DOWN	UFS_HS_G1 // sadang

	bool scale_up = false;
	int ret;

	ret = ufs_debugfs_get_user_access(hba);
	if (ret) {
		ufshcd_dump_uic_state(hba, "ufsdbg_config_pwr_mode: ufs_debugfs_get_user_access FAIL!");
		return ret;
	}

#if 1 //
	ufshcd_dump_uic_state(hba, "ufsdbg_config_pwr_mode: before hold");
#endif

	ufshcd_hold(hba);

#if 1 //
	ufshcd_dump_uic_state(hba, "ufsdbg_config_pwr_mode: after hold");
#endif

	down_write(&hba->clk_scaling_lock);
	ufshcd_scsi_block_requests(hba);

	if (ufshcd_wait_for_doorbell_clr(hba, DOORBELL_CLR_TOUT_US)) {
		ret = -EBUSY;
		goto out;
	}

#if 1 //
	ufshcd_dump_uic_state(hba, "ufsdbg_config_pwr_mode: after doorbell clear");
#endif

#if 0 //
	ufshcd_dump_uic_state(hba, "before manual h8 exit");
	ret = ufshcd_uic_hibern8_exit(hba);
	ufshcd_dump_uic_state(hba, "after manual h8 exit");

	if (ret) {
		dev_err(hba->dev, "hibern8 exit, manual FAIL! ret=%d\n\n", ret);
		ret = -EBUSY;
		goto out;
	}
#endif

#if 1 // CHANGED 1: MTK's negotiation takes FAST_MODE (HS) or SLOW_MODE (PWM);
      // FASTAUTO/SLOWAUTO make it fail and the PMC goes out all-zero.
	if (desired_pwr_mode->pwr_rx == FASTAUTO_MODE)
		desired_pwr_mode->pwr_rx = FAST_MODE;
	else if (desired_pwr_mode->pwr_rx == SLOWAUTO_MODE)
		desired_pwr_mode->pwr_rx = SLOW_MODE;
	desired_pwr_mode->pwr_tx = desired_pwr_mode->pwr_rx;
#endif

	/* Gear scaling needs to be taken care of along with clk scaling */
	if (desired_pwr_mode->gear_tx != hba->pwr_info.gear_tx ||
	    desired_pwr_mode->gear_rx != hba->pwr_info.gear_rx) {

		if (desired_pwr_mode->gear_tx > UFS_MIN_GEAR_TO_SCALE_DOWN ||
		    desired_pwr_mode->gear_rx > UFS_MIN_GEAR_TO_SCALE_DOWN)
			scale_up = true;

		if (!scale_up) {
			ret = ufshcd_config_pwr_mode(hba, desired_pwr_mode);
			if (ret)
				goto out;
		}

		ret = ufshcd_scale_clks(hba, scale_up);
		if (ret)
			goto out;

		if (scale_up) {
			ret = ufshcd_config_pwr_mode(hba, desired_pwr_mode);
			/*
			 * Safe: a failed PMC schedules the error handler, whose
			 * host reset calls ufshcd_scale_clks(hba, true) before
			 * bringing the link back up (android15-6.6).
			 */
			if (ret)
				ufshcd_scale_clks(hba, false);
		}

	} else {
		ret = ufshcd_config_pwr_mode(hba, desired_pwr_mode);
	}

	/* CHANGED 2: AH8 disable failed in MTK's PRE_CHANGE: host reset queued */
	if (!ret && hba->ufshcd_state != UFSHCD_STATE_OPERATIONAL)
		ret = -EIO;
out:
	ufshcd_scsi_unblock_requests(hba);
	up_write(&hba->clk_scaling_lock);
	ufshcd_release(hba);
	ufs_debugfs_put_user_access(hba);

	return ret;
}

static ssize_t ufsdbg_power_mode_write(struct file *file,
				const char __user *ubuf, size_t cnt,
				loff_t *ppos)
{
	struct ufs_hba *hba = file->f_mapping->host->i_private;
	struct ufs_pa_layer_attr pwr_mode;
	struct ufs_pa_layer_attr final_pwr_mode;
	char pwr_mode_str[BUFF_LINE_SIZE] = {0};
	loff_t buff_pos = 0;
	int ret;
	int idx = 0;

	ret = simple_write_to_buffer(pwr_mode_str, BUFF_LINE_SIZE,
		&buff_pos, ubuf, cnt);

	pwr_mode.gear_rx = pwr_mode_str[idx++] - '0';
	pwr_mode.gear_tx = pwr_mode_str[idx++] - '0';
	pwr_mode.lane_rx = pwr_mode_str[idx++] - '0';
	pwr_mode.lane_tx = pwr_mode_str[idx++] - '0';
	pwr_mode.pwr_rx = pwr_mode_str[idx++] - '0';
	pwr_mode.pwr_tx = pwr_mode_str[idx++] - '0';

	/*
	 * Switching between rates is not currently supported so use the
	 * current rate.
	 * TODO: add rate switching if and when it is supported in the future
	 */
	pwr_mode.hs_rate = hba->pwr_info.hs_rate;

	/* Validate user input */
	if (!ufsdbg_power_mode_validate(&pwr_mode))
		return -EINVAL;

	pr_debug("%s: new power mode requested [RX,TX]: Gear=[%d,%d], Lane=[%d,%d], Mode=[%d,%d]\n",
		__func__,
		pwr_mode.gear_rx, pwr_mode.gear_tx, pwr_mode.lane_rx,
		pwr_mode.lane_tx, pwr_mode.pwr_rx, pwr_mode.pwr_tx);

	ret = ufsdbg_cfg_pwr_param(hba, &pwr_mode, &final_pwr_mode);
	if (ret) {
		dev_err(hba->dev,
			"%s: failed to configure new power parameters, ret = %d\n",
			__func__, ret);
		return cnt;
	}

	ret = ufsdbg_config_pwr_mode(hba, &final_pwr_mode);
	if (ret == -EBUSY)
		dev_err(hba->dev,
			"%s: ufshcd_config_pwr_mode failed: system is busy, try again\n",
			__func__);
	else if (ret)
		dev_err(hba->dev,
			"%s: ufshcd_config_pwr_mode failed, ret=%d\n",
			__func__, ret);

	return cnt;
}
