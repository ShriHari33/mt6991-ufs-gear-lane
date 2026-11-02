// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm downstream "power_mode" debugfs: the code the MediaTek port started from.
 *
 * Verbatim from msm-4.19 drivers/scsi/ufs/ufs-debugfs.c lines 1092-1331,
 * public copy: LineageOS/android_kernel_xiaomi_sm8250, branch lineage-21.
 *
 * Compare ufsdbg_config_pwr_mode() below with the ported debugfs code:
 *   pm_runtime_get_sync(hba->dev)   on 4.19 this resumed the HBA *and* took
 *                                   the link out of hibern8; on 5.14+ it only
 *                                   powers the controller (runtime PM moved to
 *                                   the device W-LUN, commit b294ff3e3449)
 *   ufshcd_hold / down_write(&hba->lock) / block / wait doorbell
 *                                   hba->lock is a Qualcomm-only rwsem
 *   gear-scaling block              ufshcd_scale_clks() + manual hibern8
 *                                   enter/exit (hibern8_on_idle is Qualcomm-only)
 *   ufshcd_change_power_mode()      skips the vendor PRE_CHANGE hook
 *
 * Input format: "GGLLPP" = gear rx, gear tx, lane rx, lane tx, pwr rx, pwr tx;
 * pwr may be 1 (FAST) or 4 (FASTAUTO). On MTK only 1 works (see 05_*.c).
 */

static bool ufsdbg_power_mode_validate(struct ufs_pa_layer_attr *pwr_mode)
{
	if (pwr_mode->gear_rx < UFS_HS_G1 || pwr_mode->gear_rx > UFS_HS_G4 ||
	    pwr_mode->gear_tx < UFS_HS_G1 || pwr_mode->gear_tx > UFS_HS_G4 ||
	    pwr_mode->lane_rx < 1 || pwr_mode->lane_rx > 2 ||
	    pwr_mode->lane_tx < 1 || pwr_mode->lane_tx > 2 ||
	    (pwr_mode->pwr_rx != FAST_MODE &&
	     pwr_mode->pwr_rx != FASTAUTO_MODE) ||
	    (pwr_mode->pwr_tx != FAST_MODE &&
	     pwr_mode->pwr_tx != FASTAUTO_MODE)) {
		pr_err("%s: power parameters are not valid\n", __func__);
		return false;
	}

	return true;
}

static int ufsdbg_cfg_pwr_param(struct ufs_hba *hba,
				struct ufs_pa_layer_attr *new_pwr,
				struct ufs_pa_layer_attr *final_pwr)
{
	int ret = 0;
	bool is_dev_sup_hs = false;
	bool is_new_pwr_hs = false;
	int dev_pwm_max_rx_gear;
	int dev_pwm_max_tx_gear;

	if (!hba->max_pwr_info.is_valid) {
		dev_err(hba->dev, "%s: device max power is not valid. can't configure power\n",
			__func__);
		return -EINVAL;
	}

	if (hba->max_pwr_info.info.pwr_rx == FAST_MODE)
		is_dev_sup_hs = true;

	if (new_pwr->pwr_rx == FAST_MODE || new_pwr->pwr_rx == FASTAUTO_MODE)
		is_new_pwr_hs = true;

	final_pwr->lane_rx = hba->max_pwr_info.info.lane_rx;
	final_pwr->lane_tx = hba->max_pwr_info.info.lane_tx;

	/* device doesn't support HS but requested power is HS */
	if (!is_dev_sup_hs && is_new_pwr_hs) {
		pr_err("%s: device doesn't support HS. requested power is HS\n",
			__func__);
		return -ENOTSUPP;
	} else if ((is_dev_sup_hs && is_new_pwr_hs) ||
		   (!is_dev_sup_hs && !is_new_pwr_hs)) {
		/*
		 * If device and requested power mode are both HS or both PWM
		 * then dev_max->gear_xx are the gears to be assign to
		 * final_pwr->gear_xx
		 */
		final_pwr->gear_rx = hba->max_pwr_info.info.gear_rx;
		final_pwr->gear_tx = hba->max_pwr_info.info.gear_tx;
	} else if (is_dev_sup_hs && !is_new_pwr_hs) {
		/*
		 * If device supports HS but requested power is PWM, then we
		 * need to find out what is the max gear in PWM the device
		 * supports
		 */

		ufshcd_dme_get(hba, UIC_ARG_MIB(PA_MAXRXPWMGEAR),
			       &dev_pwm_max_rx_gear);

		if (!dev_pwm_max_rx_gear) {
			pr_err("%s: couldn't get device max pwm rx gear\n",
				__func__);
			ret = -EINVAL;
			goto out;
		}

		ufshcd_dme_peer_get(hba, UIC_ARG_MIB(PA_MAXRXPWMGEAR),
				    &dev_pwm_max_tx_gear);

		if (!dev_pwm_max_tx_gear) {
			pr_err("%s: couldn't get device max pwm tx gear\n",
				__func__);
			ret = -EINVAL;
			goto out;
		}

		final_pwr->gear_rx = dev_pwm_max_rx_gear;
		final_pwr->gear_tx = dev_pwm_max_tx_gear;
	}

	if ((new_pwr->gear_rx > final_pwr->gear_rx) ||
	    (new_pwr->gear_tx > final_pwr->gear_tx) ||
	    (new_pwr->lane_rx > final_pwr->lane_rx) ||
	    (new_pwr->lane_tx > final_pwr->lane_tx)) {
		pr_err("%s: (RX,TX) GG,LL: in PWM/HS new pwr [%d%d,%d%d] exceeds device limitation [%d%d,%d%d]\n",
			__func__,
			new_pwr->gear_rx, new_pwr->gear_tx,
			new_pwr->lane_rx, new_pwr->lane_tx,
			final_pwr->gear_rx, final_pwr->gear_tx,
			final_pwr->lane_rx, final_pwr->lane_tx);
		return -ENOTSUPP;
	}

	final_pwr->gear_rx = new_pwr->gear_rx;
	final_pwr->gear_tx = new_pwr->gear_tx;
	final_pwr->lane_rx = new_pwr->lane_rx;
	final_pwr->lane_tx = new_pwr->lane_tx;
	final_pwr->pwr_rx = new_pwr->pwr_rx;
	final_pwr->pwr_tx = new_pwr->pwr_tx;
	final_pwr->hs_rate = new_pwr->hs_rate;

out:
	return ret;
}

static int ufsdbg_config_pwr_mode(struct ufs_hba *hba,
		struct ufs_pa_layer_attr *desired_pwr_mode)
{
	int ret = 0;
	bool scale_up = false;
	u32 scale_down_gear = ufshcd_vops_get_scale_down_gear(hba);

	pm_runtime_get_sync(hba->dev);
	/* let's not get into low power until clock scaling is completed */
	hba->ufs_stats.clk_hold.ctx = DBGFS_CFG_PWR_MODE;
	ufshcd_hold(hba, false);
	down_write(&hba->lock);
	ufshcd_scsi_block_requests(hba);
	if (ufshcd_wait_for_doorbell_clr(hba, DOORBELL_CLR_TOUT_US)) {
		ret = -EBUSY;
		goto out;
	}

	/* Gear scaling needs to be taken care of along with clk scaling */
	if (desired_pwr_mode->gear_tx != hba->pwr_info.gear_tx ||
	    desired_pwr_mode->gear_rx != hba->pwr_info.gear_rx) {

		if (desired_pwr_mode->gear_tx > scale_down_gear ||
		    desired_pwr_mode->gear_rx > scale_down_gear)
			scale_up = true;

		if (!scale_up) {
			ret = ufshcd_change_power_mode(hba, desired_pwr_mode);
			if (ret)
				goto out;
		}

		/*
		 * If auto hibern8 is supported then put the link in
		 * hibern8 manually, this is to avoid auto hibern8
		 * racing during clock frequency scaling sequence.
		 */
		if (ufshcd_is_auto_hibern8_supported(hba) &&
		    hba->hibern8_on_idle.is_enabled) {
			ret = ufshcd_uic_hibern8_enter(hba);
			if (ret)
				goto out;
		}

		ret = ufshcd_scale_clks(hba, scale_up);
		if (ret)
			goto out;

		if (ufshcd_is_auto_hibern8_supported(hba) &&
		    hba->hibern8_on_idle.is_enabled)
			ret = ufshcd_uic_hibern8_exit(hba);

		if (scale_up) {
			ret = ufshcd_change_power_mode(hba, desired_pwr_mode);
			if (ret)
				ufshcd_scale_clks(hba, false);
		}
	} else {
		ret = ufshcd_change_power_mode(hba, desired_pwr_mode);
	}
out:
	up_write(&hba->lock);
	ufshcd_scsi_unblock_requests(hba);
	ufshcd_release(hba, false);
	pm_runtime_put_sync(hba->dev);

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

