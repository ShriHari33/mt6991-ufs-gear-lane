// SPDX-License-Identifier: GPL-2.0
/*
 * ufsdbg_config_pwr_mode() for kernel-6.6/drivers/ufs/core/ufs-debugfs.c
 * (GKI core, no access to struct ufs_mtk_host).
 *
 * Same shape as the ported one (04_qcom_*.c lineage), with three changes:
 *
 *  1. pm_runtime_get_sync(hba->dev) -> ufs_debugfs_get_user_access(hba)
 *     That helper is already in this file. It takes host_sem and calls
 *     ufshcd_rpm_get_sync(), which resumes the device W-LUN: that is the
 *     path that sends DME_HIBERNATE_EXIT, powers UniPro back up and restores
 *     AHIT. pm_runtime_get_sync(hba->dev) only powers the controller.
 *
 *  2. No clock-scaling block (optional: keeping the port's ufshcd_scale_clks()
 *     won't cause the hibern8 failure). On MT6991, ufs_mtk_clk_scale_notify()
 *     returns at once because the DT flag mediatek,ufs-clkscale-only-by-user
 *     leaves UFSHCD_CAP_CLK_SCALING unset, so ufshcd_scale_clks() is only
 *     the core's clk_set_rate() on ufs_sel, guarded by clki->curr_freq:
 *       scale up:   curr_freq is already max since probe -> does nothing
 *       scale down: clk_set_rate(ufs_sel, 273 MHz), without MTK's vcore,
 *                   FDE clock and pm_qos steps
 *     MTK's clkscale_control switches the mux with clk_set_parent() and
 *     never updates curr_freq, so the two don't see each other: after
 *     clkscale_control 1/3, a "scale up" here skips and G5 runs on 273 MHz.
 *     If you keep it, don't mix it with clkscale_control.
 *     Drop the manual hibern8 enter/exit that Qualcomm put around it: that
 *     runs with auto-hibern8 still on, and MTK turns AH8 off before any
 *     manual hibern8 command.
 *     Nothing to do by default: MT6991 boots with clkscale_control = 2
 *     (ufs_sel at 499.2 MHz), which G5 needs and lower gears tolerate.
 *     Only if you wrote 1 or 3 to .../clkscale/clkscale_control (273 MHz),
 *     write 2 before asking for G5. "cat" it: "current: 2" or "4" = max.
 *
 *  3. FASTAUTO/SLOWAUTO mapped to FAST/SLOW, and a queued host reset
 *     reported as -EIO (see the comments in the code).
 *
 * ufshcd_scsi_block_requests(), ufshcd_scsi_unblock_requests() and
 * ufshcd_wait_for_doorbell_clr() are static in ufshcd.c on android15-6.6;
 * expose them through ufshcd-priv.h the same way the ported code does.
 *
 * Not compiled.
 */

static int ufsdbg_config_pwr_mode(struct ufs_hba *hba,
				  struct ufs_pa_layer_attr *pwr)
{
	int ret;

	/* host_sem + ufshcd_rpm_get_sync(): link out of hibern8 on 6.6 */
	ret = ufs_debugfs_get_user_access(hba);
	if (ret)
		return ret;

	ufshcd_hold(hba);
	down_write(&hba->clk_scaling_lock);
	ufshcd_scsi_block_requests(hba);

	ret = ufshcd_wait_for_doorbell_clr(hba, 1000 * 1000); /* 1 sec */
	if (ret)
		goto out;

	/*
	 * MTK's negotiation in ufs_mtk_pre_pwr_change() accepts FAST_MODE (HS)
	 * or SLOW_MODE (PWM). FASTAUTO/SLOWAUTO make it fail, the failure is
	 * only logged, and the PMC goes out with gear/lane/mode all zero.
	 */
	if (pwr->pwr_rx == FASTAUTO_MODE)
		pwr->pwr_rx = FAST_MODE;
	else if (pwr->pwr_rx == SLOWAUTO_MODE)
		pwr->pwr_rx = SLOW_MODE;
	pwr->pwr_tx = pwr->pwr_rx;

	ret = ufshcd_config_pwr_mode(hba, pwr);

	/*
	 * If the auto-hibern8 disable in MTK's PRE_CHANGE failed, the vendor
	 * code has already queued a host reset, which will put the link back
	 * to max gear/lanes whatever the PMC returned.
	 */
	if (!ret && hba->ufshcd_state != UFSHCD_STATE_OPERATIONAL)
		ret = -EIO;

	dev_info(hba->dev, "%s: ret=%d [RX, TX]: gear=[%d, %d], lane=[%d, %d]\n",
		 __func__, ret, hba->pwr_info.gear_rx, hba->pwr_info.gear_tx,
		 hba->pwr_info.lane_rx, hba->pwr_info.lane_tx);
out:
	ufshcd_scsi_unblock_requests(hba);
	up_write(&hba->clk_scaling_lock);
	ufshcd_release(hba);
	ufs_debugfs_put_user_access(hba);

	return ret;
}
