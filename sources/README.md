# Source copies the write-up cites

Every `file:line` in the write-ups points into these copies. Citations were
checked against them: 42 in `UFS_gear_lane_fix_explained_v1` (31 October 2026),
80 in `UFS_gear_lane_fix_explained` (1 November), 82 in
`UFS_MT6991_bug_fix_and_G5` (1 November) and 46 in
`UFS_MT6991_encryption_check` (1 November).

These are reference copies, not the board's MediaTek SoC BSP. Line numbers in
a vendor tree will differ, especially in `ufshcd.c`, which carries the port's
changes there, so search by function name:
`grep -n "ufshcd_change_power_mode" drivers/ufs/core/ufshcd.c`

| Folder | Where it came from | PDF citations that point here |
|---|---|---|
| `android15-6.6/` | Android common kernel, android15-6.6: `drivers/ufs/core/` (`ufshcd.c`, `ufs-debugfs.c`, `ufshcd-priv.h`) and `include/ufs/ufshcd.h` | `ufshcd.c:…` (except Part 4), `ufshcd-priv.h`, `ufshcd.h`, `ufs-debugfs.c:68` |
| `mtk_vendor_6.6/` | A public MT6991 vendor kernel release, `kernel_device_modules-6.6` | `ufs-mediatek*.c/.h`, `mt6991.dts` |
| `upstream_v5.13_v5.14/` | Linux v5.13 and v5.14, `drivers/scsi/ufs/ufshcd.c` | Part 4: `ufshcd.c:8846` is in the v5.13 file |
| `qcom_msm-4.19/` | LineageOS sm8250 msm-4.19, `drivers/scsi/ufs/ufs-debugfs.c` | `ufs-debugfs.c:1211` and `:1237` |
| `upstream_v6.18/` | Upstream Linux 6.18 (shallow clone, 5 December 2025), eight files kept at their kernel paths: fscrypt, f2fs, blk-crypto sysfs, UFS crypto, arm64 AES | Everything prefixed `upstream_v6.18/` in `UFS_MT6991_encryption_check`. Used because the matching `android15-6.6` files aren't on this machine |

The three MediaTek files in `../vendor_6.6/` are byte-identical to the ones here.

## A power mode change in `android15-6.6/ufshcd.c`

```
ufshcd_config_pwr_mode()                  4652  the debugfs function calls this
 ├─ vendor hook, PRE_CHANGE               ufshcd-priv.h:182
 │    → MediaTek ufs_mtk_pwr_change_notify()  ufs-mediatek.c:2023
 │      saves AHIT, turns auto-hibern8 off, picks the final gear/lanes/mode
 └─ ufshcd_change_power_mode()            4560
     ├─ "power already configured": return, nothing sent   4566
     ├─ DME_SETs: PA_RXGEAR, PA_TXGEAR, lanes, termination, HS series,
     │   timeouts. Each goes ufshcd_dme_set_attr() 4074 → __ufshcd_send_uic_cmd() 2468
     ├─ ufshcd_uic_change_pwr_mode()      4333  writes PA_PWRMODE: the actual "go"
     │   └─ ufshcd_uic_pwr_ctrl()         4199  sends it, then sleeps on uic_async_done
     │        for up to uic_cmd_timeout (UIC_CMD_TIMEOUT_DEFAULT = 500 ms, line 59).
     │        Gives up with "pwr ctrl cmd … completion timeout" at 4237.
     │        Woken by the interrupt handler ufshcd_uic_cmd_compl() 5472,
     │        which calls complete() at 5493 when the power-mode interrupt arrives.
     └─ only on success: vendor hook POST_CHANGE (MediaTek writes AHIT back),
        then hba->pwr_info = the new mode   4638
```
