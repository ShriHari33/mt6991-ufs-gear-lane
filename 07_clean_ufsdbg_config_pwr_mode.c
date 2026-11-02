// SPDX-License-Identifier: GPL-2.0
/*
 * 07_clean_ufsdbg_config_pwr_mode.c
 *
 * Clean version of the runtime gear/lane/mode control in debugfs, for
 * android15-6.6 with MediaTek's vendor modules (MT6991). Not compiled:
 * check it against your tree before building.
 *
 * Three pieces, each marked with where it goes:
 *   PART A  drivers/ufs/core/ufshcd.c       a new helper, placed right after
 *                                           ufshcd_config_pwr_mode()
 *   PART B  drivers/ufs/core/ufshcd-priv.h  one declaration
 *   PART C  drivers/ufs/core/ufs-debugfs.c  replaces ufsdbg_config_pwr_mode()
 *                                           and ufsdbg_power_mode_write()
 *
 * What changes compared with the working version (file 06):
 *   - The "stop I/O, then PMC" sequence moves into one core helper (PART A).
 *     ufs-debugfs.c then needs none of ufshcd.c's static functions, so the port's
 *     exports of ufshcd_scsi_block_requests(), ufshcd_scsi_unblock_requests(),
 *     ufshcd_wait_for_doorbell_clr() and ufshcd_scale_clks() can go back to
 *     static. Grep your tree first, in case something else uses them.
 *   - The port's clock-scaling block is gone (write-up, 6.3 update). On the board
 *     it could raise ufs_sel to 499.2 MHz without the vcore that needs.
 *   - No register dumps before ufshcd_hold() (write-up, 6.2 review).
 *   - A request the vendor lowered without an error (G5 on a host that
 *     MediaTek caps at G4) now fails with -ERANGE and says what the link
 *     actually runs at.
 *   - One dev_info line on success. The core prints nothing on success.
 *   - The write handler returns the real error, so a failed echo fails.
 *   - Refuses with -EBUSY while the core's clock scaling is active, because
 *     its governor would undo the request. It's off on MT6991.
 *
 * Kept from the working version: the W-LUN wake
 * (ufs_debugfs_get_user_access()), the AUTO -> FAST/SLOW mapping, the
 * OPERATIONAL check, the six-digit input, ufsdbg_power_mode_validate()
 * (G1 to G5) and ufsdbg_cfg_pwr_param().
 *
 * To leave ufshcd.c untouched, keep the port's exports and paste the body
 * of PART A into ufsdbg_config_pwr_mode() in place of the helper call.
 */


/* ======================================================================
 * PART A: drivers/ufs/core/ufshcd.c
 * Place right after ufshcd_config_pwr_mode() and its EXPORT_SYMBOL_GPL.
 * No export needed: ufs-debugfs.c is built into the same module.
 * ====================================================================== */

/**
 * ufshcd_config_pwr_mode_quiesced - change the power mode with I/O stopped
 * @hba: per adapter instance
 * @pwr: requested gear, lanes, mode and rate
 *
 * The same steps the core takes before its own gear changes: keep the
 * clocks on, take clk_scaling_lock for writing, stop new SCSI commands and
 * wait for the ones in flight. Then run the PMC through
 * ufshcd_config_pwr_mode(), so the vendor's PRE_CHANGE and POST_CHANGE
 * hooks run.
 *
 * The caller must hold hba->host_sem and a runtime PM reference on the
 * UFS device W-LUN (ufshcd_rpm_get_sync()). Without that reference the link
 * can be in software hibern8, and the PMC fails.
 *
 * Return: 0 on success, -EBUSY if the commands in flight didn't finish
 * within a second or the host isn't operational, or the error from
 * ufshcd_config_pwr_mode().
 */
int ufshcd_config_pwr_mode_quiesced(struct ufs_hba *hba,
				    struct ufs_pa_layer_attr *pwr)
{
	int ret;

	ufshcd_hold(hba);
	down_write(&hba->clk_scaling_lock);
	ufshcd_scsi_block_requests(hba);

	ret = ufshcd_wait_for_doorbell_clr(hba, USEC_PER_SEC);
	if (!ret)
		ret = ufshcd_config_pwr_mode(hba, pwr);

	ufshcd_scsi_unblock_requests(hba);
	up_write(&hba->clk_scaling_lock);
	ufshcd_release(hba);

	return ret;
}


/* ======================================================================
 * PART B: drivers/ufs/core/ufshcd-priv.h
 * Next to the other function declarations.
 * ====================================================================== */

int ufshcd_config_pwr_mode_quiesced(struct ufs_hba *hba,
				    struct ufs_pa_layer_attr *pwr);


/* ======================================================================
 * PART C: drivers/ufs/core/ufs-debugfs.c
 * Replaces ufsdbg_config_pwr_mode() and ufsdbg_power_mode_write().
 * Needs #include <linux/ctype.h> for isdigit(), if the file lacks it.
 * BUFF_LINE_SIZE, ufsdbg_power_mode_validate() and ufsdbg_cfg_pwr_param()
 * are the ones already in your file.
 * ====================================================================== */

static bool ufsdbg_pwr_mode_differs(const struct ufs_pa_layer_attr *a,
				    const struct ufs_pa_layer_attr *b)
{
	return a->gear_rx != b->gear_rx || a->gear_tx != b->gear_tx ||
	       a->lane_rx != b->lane_rx || a->lane_tx != b->lane_tx ||
	       a->pwr_rx != b->pwr_rx || a->pwr_tx != b->pwr_tx;
}

static int ufsdbg_config_pwr_mode(struct ufs_hba *hba,
				  struct ufs_pa_layer_attr *want)
{
	const struct ufs_pa_layer_attr *now = &hba->pwr_info;
	int ret;

	/*
	 * With the core's clock scaling active, its governor changes the gear
	 * by itself and would undo this request. It's off on MT6991
	 * (MediaTek's "clkscale-only-by-user", or its 26 MHz / UFS 3.x fixup).
	 */
	if (ufshcd_is_clkscaling_supported(hba))
		return -EBUSY;

	/*
	 * MediaTek's negotiation accepts FAST (HS gears) and SLOW (PWM gears)
	 * only; the AUTO variants make it fail. Use one mode both ways.
	 */
	if (want->pwr_rx == FASTAUTO_MODE)
		want->pwr_rx = FAST_MODE;
	else if (want->pwr_rx == SLOWAUTO_MODE)
		want->pwr_rx = SLOW_MODE;
	want->pwr_tx = want->pwr_rx;

	/*
	 * Take host_sem and wake the UFS device W-LUN, which wakes the
	 * controller first. Waking only hba->dev leaves the link in software
	 * hibern8, and the PMC fails ("exit h8 state fail", or a PMC timeout).
	 */
	ret = ufs_debugfs_get_user_access(hba);
	if (ret)
		return ret;

	ret = ufshcd_config_pwr_mode_quiesced(hba, want);

	/* MediaTek's hook can fail, queue a reset and still let the PMC pass */
	if (!ret && hba->ufshcd_state != UFSHCD_STATE_OPERATIONAL)
		ret = -EIO;

	/*
	 * The vendor's negotiation can lower a request without an error.
	 * MediaTek caps the host at HS-G4 when the chip's reference clock is
	 * 26 MHz. Report what the link really runs at.
	 */
	if (!ret && ufsdbg_pwr_mode_differs(now, want)) {
		dev_warn(hba->dev,
			 "power_mode: asked [RX, TX] gear [%u, %u] lanes [%u, %u] mode [%u, %u], link runs gear [%u, %u] lanes [%u, %u] mode [%u, %u]\n",
			 want->gear_rx, want->gear_tx, want->lane_rx,
			 want->lane_tx, want->pwr_rx, want->pwr_tx,
			 now->gear_rx, now->gear_tx, now->lane_rx,
			 now->lane_tx, now->pwr_rx, now->pwr_tx);
		ret = -ERANGE;
	}

	if (!ret)
		dev_info(hba->dev,
			 "power_mode: [RX, TX] gear [%u, %u] lanes [%u, %u] mode [%u, %u]\n",
			 now->gear_rx, now->gear_tx, now->lane_rx,
			 now->lane_tx, now->pwr_rx, now->pwr_tx);

	ufs_debugfs_put_user_access(hba);
	return ret;
}

static ssize_t ufsdbg_power_mode_write(struct file *file,
				       const char __user *ubuf, size_t cnt,
				       loff_t *ppos)
{
	struct ufs_hba *hba = file->f_mapping->host->i_private;
	struct ufs_pa_layer_attr pwr_mode, final_pwr_mode;
	char buf[BUFF_LINE_SIZE] = {0};
	loff_t pos = 0;
	ssize_t len;
	int i, ret;

	len = simple_write_to_buffer(buf, sizeof(buf) - 1, &pos, ubuf, cnt);
	if (len < 0)
		return len;

	/* GGLLMM: gear RX, gear TX, lanes RX, lanes TX, mode RX, mode TX */
	if (len < 6)
		return -EINVAL;
	for (i = 0; i < 6; i++)
		if (!isdigit(buf[i]))
			return -EINVAL;

	pwr_mode.gear_rx = buf[0] - '0';
	pwr_mode.gear_tx = buf[1] - '0';
	pwr_mode.lane_rx = buf[2] - '0';
	pwr_mode.lane_tx = buf[3] - '0';
	pwr_mode.pwr_rx = buf[4] - '0';
	pwr_mode.pwr_tx = buf[5] - '0';
	/* Switching between rate A and B isn't supported: keep the current one */
	pwr_mode.hs_rate = hba->pwr_info.hs_rate;

	if (!ufsdbg_power_mode_validate(&pwr_mode))
		return -EINVAL;

	/* Rejects anything above the device's maximum gear or lane count */
	ret = ufsdbg_cfg_pwr_param(hba, &pwr_mode, &final_pwr_mode);
	if (ret)
		return ret == -ENOTSUPP ? -EOPNOTSUPP : ret;

	ret = ufsdbg_config_pwr_mode(hba, &final_pwr_mode);
	return ret ? ret : cnt;
}


/* ======================================================================
 * After building: quick checks
 *
 * P=/sys/kernel/debug/ufshcd/16810000.ufshci/power_mode  (adjust if yours
 * lives elsewhere)
 *
 *   echo 112211 > $P; echo $?      0, and dmesg shows
 *                                  "power_mode: [RX, TX] gear [1, 1] ..."
 *   echo 442211 > $P; echo $?      0, back to G4
 *   echo 552211 > $P; echo $?      on a G4 board: non-zero, and dmesg says
 *                                  either "exceeds device limitation" (UFS 3.x
 *                                  chip) or "asked ... link runs gear [4, 4]"
 *                                  (26 MHz reference clock)
 *   cat $P                         what the core recorded (hba->pwr_info)
 *   dmesg | grep "err_handler started"   should print nothing
 * ====================================================================== */
