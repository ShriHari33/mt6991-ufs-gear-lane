# Current power_mode code vs the proposed version

Nothing here is compiled yet. Build it in a kernel tree before relying on it.

## Files

| File | What it is |
|---|---|
| `current/ufs-debugfs_power_mode.c` | The power_mode code as it runs on the MT6991 board: `ufsdbg_power_mode_validate()` (the G1–G5 version from file 06), `ufsdbg_config_pwr_mode()` (as of 31 October, with the CHANGED lines from file 06 applied), and the write handler as ported from Qualcomm's msm-4.19. |
| `proposed/ufs-debugfs_power_mode.c` | The same three functions, rewritten (same as Part C of file 07). They replace the current ones in `ufs-debugfs.c`. |
| `current_vs_proposed.diff` | `diff -u` between the two files above. |
| `proposed/0001-ufs-core-add-ufshcd_config_pwr_mode_quiesced.patch` | Adds the core helper the proposed code calls (`ufshcd.c` and `ufshcd-priv.h`). |
| `proposed/0002-ufs-debugfs-add-clk_rates.patch` | Adds a read-only debugfs file, `clk_rates`, listing every UFS clock with its real rate. |
| `proposed/0003-ufs-mediatek-raise-clocks-before-entering-HS-G5.patch` | MediaTek vendor module: before any power mode change into HS-G5, puts `ufs_sel` and the encryption clock `ufs_fde` on their fast sources (vcore first). See [Vendor patch 0003](#vendor-patch-0003). |

Both patches are against stock `android15-6.6` (the copies in `../sources/android15-6.6/`), and both apply cleanly there (`patch -p1 --dry-run`). A vendor tree that already carries the ported code may need offsets or a little fuzz, so dry-run first.

To compare the two side by side, for example in VS Code:

```
code --diff current/ufs-debugfs_power_mode.c proposed/ufs-debugfs_power_mode.c
```

## What changes, and why

Section numbers refer to `UFS_MT6991_bug_fix_and_G5.pdf`.

| # | Change | Why | PDF |
|---|---|---|---|
| 1 | **Kept:** `ufs_debugfs_get_user_access()` (the W-LUN wake, under `host_sem`) | This is the fix. | 2.2 |
| 2 | **Kept:** AUTO modes mapped to FAST/SLOW | MediaTek's negotiation accepts only FAST and SLOW. | 2.3 |
| 3 | **Kept:** `-EIO` when a reset was queued | MediaTek's hook can fail and still let the PMC report success. | 2.3 |
| 4 | **Moved:** hold, `clk_scaling_lock`, block, drain, PMC now live in one core helper, `ufshcd_config_pwr_mode_quiesced()` (patch 0001) | `ufs-debugfs.c` no longer needs `ufshcd_scsi_block_requests()`, `ufshcd_scsi_unblock_requests()` and `ufshcd_wait_for_doorbell_clr()` exported. | 2.5 |
| 5 | **Removed:** the port's clock block (`UFS_MIN_GEAR_TO_SCALE_DOWN`, `scale_up`, both `ufshcd_scale_clks()` calls) | On the MT6991 board it moves `ufs_sel` in a way MediaTek never does, leaves it at 499.2 MHz at G4, and can't touch the encryption clock. `ufshcd_scale_clks()` can go back to static. | 4.10 |
| 6 | **Removed:** all `ufshcd_dump_uic_state()` calls | Two of them can read registers with the clocks off. If a print is needed, add it below `ufshcd_hold()`. | 2.4 |
| 7 | **Removed:** the `#if 0` manual hibern8 exit | The W-LUN wake does this job properly. | 2.6 |
| 8 | **New:** `-EBUSY` while the core's automatic clock scaling is active | Its governor would undo the requested gear. It's off on the MT6991 board, so no effect there. | 4.3 |
| 9 | **New:** `-ERANGE` plus a `dev_warn` when the link ends up at a different mode than requested | Catches a request the vendor lowered without an error, or a "power already configured" no-op. | 2.5, 4.7 |
| 10 | **New:** one `dev_info` line on success | The core prints nothing when a PMC succeeds. | 1.4 |
| 11 | **Changed:** write handler checks for six digits, returns the real error instead of `cnt`, and turns `-ENOTSUPP` into `-EOPNOTSUPP` | A failed `echo` now fails visibly in the shell; `-ENOTSUPP` is kernel-internal and shows up as "Unknown error 524". | 2.7 |
| 12 | **New file:** `clk_rates` (patch 0002) | Shows the clocks that were invisible: `ufs_sel` and `ufs_fde` (the encryption engine), with the core's own record next to the real rate. | 4.4 |

`ufsdbg_cfg_pwr_param()` is unchanged; keep the existing one.

## How to apply

1. Patch 0001: dry-run, then apply. Or by hand: put the helper right after `EXPORT_SYMBOL_GPL(ufshcd_config_pwr_mode);` in `drivers/ufs/core/ufshcd.c`, and its declaration in `drivers/ufs/core/ufshcd-priv.h`.
2. Patch 0002: dry-run, then apply.
3. In `drivers/ufs/core/ufs-debugfs.c`, replace `ufsdbg_config_pwr_mode()` and `ufsdbg_power_mode_write()` with the ones in `proposed/`, and add `#include <linux/ctype.h>` if it's missing. Keep the line the current write handler uses to find `hba`, if it differs from `file->f_mapping->host->i_private`.
4. Undo the port's exports, if nothing else uses them. Check first:
   ```
   grep -rn "ufshcd_scale_clks\|ufshcd_scsi_block_requests\|ufshcd_scsi_unblock_requests\|ufshcd_wait_for_doorbell_clr\|DOORBELL_CLR_TOUT_US" drivers/ufs/
   ```
   If the only users are in `ufshcd.c`, make them `static` again and remove their declarations.
5. Build the kernel image and flash it.

## Checks after building

```
P=/sys/kernel/debug/ufshcd/16810000.ufshci/power_mode   # adjust if it lives elsewhere
echo 112211 > $P; echo $?     # 0, dmesg: "power_mode: [RX, TX] gear [1, 1] ..."
echo 442211 > $P; echo $?     # 0, back to G4
echo 552211 > $P; echo $?     # 0 at G5, or an error that says why not
cat $P                        # what the core recorded (hba->pwr_info)
cat /sys/kernel/debug/ufshcd/16810000.ufshci/clk_rates
dmesg | grep "err_handler started"   # should print nothing
```

In `clk_rates`, look at two rows. `ufs_sel` should show 499200000 while the disk is busy at G5, with `dt_min`/`dt_max` 273000000/499200000. `ufs_fde` shows 416000000 or 546000000, with 0/0 in the device-tree columns because the core doesn't scale it.

## Vendor patch 0003

Without it, MediaTek raises `ufs_sel` and `ufs_fde` only from `clkscale_control` 2/4 (`ufs-mediatek.c:2332–2336`, clocks before the gear) and from its resume hook when the disk wakes at G5 (`:2697–2699`). A G5 request through `power_mode` goes through `ufs_mtk_pre_pwr_change()` (`:1908`), which negotiates the gear but never touches the clocks. So with the disk kept awake, G5 can run on 273 MHz and 416 MHz for as long as it stays awake.

The patch adds the scale-up to `ufs_mtk_pre_pwr_change()`, after the gear is negotiated and before any PMC, whenever the result is HS-G5. The core calls that hook before every PMC (`ufshcd.c:4658`), even for a request the link already matches, so it covers `power_mode`, error recovery and the PMC after link startup. Nothing is lowered: the G5 suspend hook already drops the clocks while the disk sleeps.

It applies to `../sources/mtk_vendor_6.6/ufs-mediatek.c` (paths are relative to the vendor tree, `drivers/ufs/`). It changes the MediaTek UFS vendor module, not the kernel image. Not compiled yet.

Check after flashing, with no `clkscale_control` writes since boot:

```
grep -w -B8 ufs_fde /sys/kernel/debug/clk/clk_summary   # ck_aes_ufsfde_sel 546000000 under ck_mainpll_d4
P=/sys/kernel/debug/ufshcd/16810000.ufshci/power_mode
C=/sys/bus/platform/devices/16810000.ufshci/clkscale/clkscale_control
echo 1 > $C                     # MediaTek scale-down: G4, ufs_sel 273 MHz, ufs_fde 416 MHz
echo 552211 > $P                # G5 through power_mode
grep -w -B8 ufs_fde /sys/kernel/debug/clk/clk_summary   # back on 546000000 at once, no sleep/wake needed
```

## Sending it upstream

Patches 0001 and 0002 are written against stock files on purpose: that is the shape any patch submission needs. The power_mode code itself isn't in stock 6.6 or in mainline.
