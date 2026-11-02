// SPDX-License-Identifier: GPL-2.0
/*
 * PROPOSED: replaces the three functions in current/. Needs:
 *   - ufshcd_config_pwr_mode_quiesced() from 0001 (ufshcd.c, ufshcd-priv.h)
 *   - #include <linux/ctype.h> in ufs-debugfs.c, for isdigit()
 * ufsdbg_power_mode_validate() is unchanged. ufsdbg_cfg_pwr_param() stays
 * as it is in your file.
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
