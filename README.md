# UFS gear and lane switching on MediaTek MT6991 (Linux 6.6)

A debugfs file, `power_mode`, ported from Qualcomm's msm-4.19 kernel, lets you
change the UFS link's gear, lane count and power mode at runtime. On an MT6991
evaluation board (Android GKI `android15-6.6` with MediaTek vendor modules) it
failed with a PMC timeout, and then with `exit h8 state fail`, whenever the
board had been idle for about half a second.

**Cause:** since Linux 5.14, the UFS chip side (the W-LUN) runtime-resumes
separately from the host controller. The ported code woke only the controller,
so the power mode change went into a link that was still in hibern8.
**Fix:** wake the W-LUN with `ufs_debugfs_get_user_access()`, which calls
`ufshcd_rpm_get_sync()`.

After the fix, this folder also covers an investigation into inconsistent G5
write throughput, including the UFS controller's AES encryption engine and its
clock.

## Read first

| PDF | What it is |
|---|---|
| `UFS_MT6991_bug_fix_and_G5.pdf` | The main document (49 pages): the bug, the fix, why it happened, and the G5 investigation. |
| `UFS_MT6991_encryption_check.pdf` | Companion (11 pages): four read-only checks for whether `/data` is encrypted, and whether the UFS AES engine or the CPU does the work. |
| `UFS_gear_lane_fix_explained.pdf`, `UFS_gear_lane_fix_explained_v1.pdf` | Earlier versions of the first write-up, kept as history. |

`read_in_order/` has numbered copies of the same PDFs and a reading plan.
Every PDF has an `.html` twin with the same content. GitHub shows `.html`
files as source code, so read the PDFs on GitHub, or turn on GitHub Pages to
view the HTML.

## Folder map

```
.
├── README.md                          this file
├── UFS_MT6991_bug_fix_and_G5.pdf/.html        main document
├── UFS_MT6991_encryption_check.pdf/.html      companion: encryption checks
├── UFS_gear_lane_fix_explained.pdf/.html      history, 1 November version
├── UFS_gear_lane_fix_explained_v1.pdf/.html   history, 31 October version
├── read_in_order/                     numbered copies of the PDFs, README.txt
│
├── 01_mtk_reference.c                 MediaTek's own power-mode wrapper, verbatim, for comparison
├── 02_ufs_mtk_config_gear_lane.c      earlier approach: gear/lane helper in the MediaTek driver
├── 03_gear_lane_sysfs.c               earlier approach: its sysfs file
├── 0001-ufs-mediatek-add-gear_lane-sysfs.patch   earlier approach as a patch (not used)
├── 04_qcom_msm-4.19_ufsdbg_config_pwr_mode.c    Qualcomm's original, the code that was ported
├── 05_core_ufs-debugfs_config_pwr_mode.c        first draft of a core replacement
├── 06_ported_ufsdbg_config_pwr_mode_fixed.c     the ported function as it ran on the board, with review fixes
├── 07_clean_ufsdbg_config_pwr_mode.c            clean rewrite (not compiled yet)
│
├── current_vs_proposed/
│   ├── README.md                      the 12 changes, with reasons and apply steps
│   ├── current/ufs-debugfs_power_mode.c    code running on the board
│   ├── proposed/ufs-debugfs_power_mode.c   proposed replacement
│   ├── proposed/0001-ufs-core-add-ufshcd_config_pwr_mode_quiesced.patch
│   ├── proposed/0002-ufs-debugfs-add-clk_rates.patch
│   ├── proposed/0003-ufs-mediatek-raise-clocks-before-entering-HS-G5.patch   (vendor module)
│   └── current_vs_proposed.diff
│
├── patched_6.6/                       MediaTek driver with the 0001 patch applied
├── vendor_6.6/                        byte-identical copies of three files in sources/mtk_vendor_6.6/
│
└── sources/                           unmodified reference copies that every file:line cites
    ├── README.md                      where each copy came from
    ├── android15-6.6/                 ufshcd.c, ufshcd.h, ufshcd-priv.h, ufs-debugfs.c
    ├── mtk_vendor_6.6/                ufs-mediatek*.c/.h, phy-mtk-ufs.c, mt6991.dts
    ├── qcom_msm-4.19/                 ufs-debugfs.c
    ├── upstream_v5.13_v5.14/          ufshcd.c before and after the 5.14 W-LUN change
    └── upstream_v6.18/                fscrypt, f2fs, blk-crypto, UFS crypto, arm64 AES files
```

## Status, 1 November 2026

- **Seen on the board:** the fix works. No more PMC failures, and lane changes from 2 to 1 work. G5 really runs. The controller clock drops to 273 MHz only while the disk sleeps at G5. During a steady 3000 MB/s run, the encryption engine's clock sat at 416 MHz instead of 546.
- **Not yet proven:** that the encryption clock is what limits G5 writes (main document, section 4.11), and that `/data` uses the hardware engine at all (companion, section 2).
- **Not compiled yet:** `07_clean_ufsdbg_config_pwr_mode.c` and the two patches in `current_vs_proposed/proposed/`. Both patches apply cleanly to `sources/android15-6.6`.

## Sources and licences

The kernel files in `sources/` are unmodified GPL-2.0 copies from public trees:
the Android common kernel `android15-6.6`, OnePlus's public MT6991 tree
(`kernel_device_modules-6.6`), LineageOS sm8250 msm-4.19, and upstream Linux
v5.13, v5.14 and 6.18. `sources/README.md` says which file came from where.
The code written for this investigation carries `SPDX-License-Identifier:
GPL-2.0` headers.
