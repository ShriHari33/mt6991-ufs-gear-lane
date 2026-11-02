Read in this order
==================

1_START_HERE_UFS_MT6991_bug_fix_and_G5.pdf   (48 pages, 1 November)
    The main document. It contains everything in 3 and 4,
    plus the whole G5 investigation.

2_UFS_MT6991_encryption_check.pdf   (11 pages, 1 November)
    Companion to section 4.5 of 1. Four read-only checks: is /data
    encrypted, and does the UFS controller's AES engine or the CPU
    do the work. Run it before the decisive run (4.11 in 1), because
    the answer decides whether the encryption clock can matter at all.

3_optional_history_UFS_gear_lane_fix_explained_1Nov.pdf   (45 pages)
4_optional_history_UFS_gear_lane_fix_explained_v1_31Oct.pdf   (33 pages)
    Earlier versions of the first write-up, kept as history.
    Nothing in them is missing from 1.


Reading plan for 1 and 2, in sittings
-------------------------------------
Sitting 1   page 1 of 1 (the whole story on one page), then Part 1, the bug (pages 3-8)
Sitting 2   Part 2 of 1, how it was found and what changed, with the tests (pages 9-15)
Sitting 3   Part 3 of 1, why it happened (pages 16-27). The densest part.
            If you only read three sections: 3.1, 3.2 and 3.5.
Sitting 4   Part 4 of 1, what we have now: the G5 investigation (pages 28-38).
Sitting 5   All of 2 (11 pages). Then run its four checks on the board,
            and after that 4.11 in 1.
Keep open   Part 6 of 1, the command cheat sheet (pages 41-42),
            and section 2 of 2, the four checks (pages 4-7).
Look up     Part 5 of 1 (corrections along the way), 7 (glossary), 8 (files), 9 (references).

These are copies. If a PDF is rebuilt in the parent folder, copy it here again.
