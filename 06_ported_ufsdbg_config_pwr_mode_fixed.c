// SPDX-License-Identifier: GPL-2.0
/*
 * The ported ufsdbg_config_pwr_mode() (kernel-6.6/drivers/ufs/core/ufs-debugfs.c),
 * as of 31 October, with the review fixes applied.
 * Clock-scaling logic and debug prints are kept as they were.
 *
 * Lines marked "CHANGED" are the only differences from the ported version.
 * The rest was copied by hand and may differ from the original in small
 * ways, so apply the CHANGED lines to the original rather than replacing
 * the whole function.
 *
 *   CHANGED 1: pwr->pwr_rx / pwr->pwr_tx -> desired_pwr_mode (compile error:
 *              there is no "pwr" in this function), and map only the AUTO
 *              modes instead of forcing FAST_MODE, so PWM requests stay PWM.
 *   CHANGED 2: report a queued host reset as -EIO (optional).
 *
 * Also check (outside this function):
 *   - ufsdbg_power_mode_validate(): the Qualcomm original caps gear at G4
 *     and accepts only FAST/FASTAUTO. On MT6991 a G5 request, or any PWM
 *     request, fails there with "power parameters are not valid". A version
 *     that allows both is at the bottom of this file.
 *   - ufsdbg_cfg_pwr_param(): "hba->max_pwr_info.info.pwr_rx = FAST_MODE;"
 *     writes core state that the error handler uses to restore the link.
 *     The board's UFS 4.0 device already has FAST_MODE there, so no effect;
 *     don't write anything else into it.
 *
 * Not compiled.
 */

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

/*
 * Only if your ufsdbg_power_mode_validate() is still the Qualcomm one
 * (gear <= G4, FAST/FASTAUTO only): this allows HS-G5 and PWM.
 * ufsdbg_cfg_pwr_param() still caps the request at the device's maximum.
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
