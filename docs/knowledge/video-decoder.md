# MT6582 hardware video decoder: what the stock firmware does

Status: **reverse-engineering notes, 2026-11-12** (video plan step 1). Read from the stock
`y2_v3.2.0_FM-20260813` kernel (`out/rea/fm-kernel.elf`, symbols present) and from `/system/lib/libvcodecdrv.so`
(not stripped; decompiled with Ghidra into `out/rea/video/decomp/vcodecdrv/`, one file per function). Nothing in
this file has run on the player unless a line says so; the bring-up section near the end is measured.

Extraction: `system.img` is an Android sparse image; convert it to raw, then
`debugfs -R "dump /lib/libvcodecdrv.so libvcodecdrv.so" system.raw`. Ghidra loads the library at `0x10000`, so a
`DAT_000xxxxx` literal sits at file address `xxxxx - 0x10000`.

## Kernel side (stock `videocodec` driver, `/dev/Vcodec`)

| Fact | Value | Where it was read |
|---|---|---|
| Clock/reset block ("SYS", VDEC_GCON) | `0x16000000` | `vdec_power_on` reads `0xF6000000`; `H264_HAL_WriteSYS` |
| SMI larb of the decoder (larb1) | `0x16010000` | `larb_clock_on`, MT6582 map |
| Decoder registers | `0x16020000`–`0x16028fff` | `vcodec_mmap` admits `0x16000000`..`0x16029000` |
| Interrupt | GIC hwirq 171 (SPI 139), level, active low | `vcodec_driver_init`: `request_irq(0xab, …, IRQF_TRIGGER_LOW)`; `vcodec_pm_restore_noirq` |
| Encoder (not used by us) | `0x15009000`, hwirq 172 | same functions |
| Clocks on power-up, in order | `MT_CG_DISP0_SMI_COMMON` (id 96), `MT_CG_VDEC0_VDEC` (256), `MT_CG_VDEC1_LARB` (288) | `vdec_power_on` |
| After power-up | `SYS+0x0` reads `0x111` when the clocks are on (stock logs otherwise); `SYS+0x8` is the larb gate | `vdec_power_on` |
| Power domain | SPM `VDE_PWR_CON` = SPM `+0x210`, status bit 7 in `PWR_STATUS` (`+0x60c`) and `PWR_STATUS_S` (`+0x610`) | `spm_mtcmos_ctrl_vdec` |
| Mux | `CLK_TOP_VDEC_SEL` (CLK_CFG_1, bits 8..11, gate bit 15) | our clock driver already names it |

Power domain on (`spm_mtcmos_ctrl_vdec(1)`), all on `VDE_PWR_CON`: set `PWR_ON` (bit 2), set `PWR_ON_S` (bit 3),
wait until bit 7 is set in both status registers, clear `PWR_CLK_DIS` (bit 4), clear `PWR_ISO` (bit 1), set
`PWR_RST_B` (bit 0), clear `SRAM_PDN` (bits 8..11), wait until `SRAM_PDN_ACK` (bit 12) reads 0. Then stock restores
the larb registers (`larb_restore`). Off is the mirror: set `SRAM_PDN`, wait for ack, set `PWR_ISO`, clear
`PWR_RST_B` and set `PWR_CLK_DIS`, clear `PWR_ON` and `PWR_ON_S`, wait until bit 7 is clear in both.

Interrupt handler (`dec_isr`): the status is `MISC+0xa4` bit 16. Acknowledge: set bits 0 and 4 (`|= 0x11`), then
clear bit 4. The handler then wakes the waiting decoder thread; nothing else happens in the kernel.

The stock kernel driver does nothing else for decoding: a lock (one decoder user at a time; taking it switches
the clocks on and enables the interrupt, releasing it switches them off), wait-for-interrupt with a timeout, and
`mmap` of the registers into the process. All decoding logic is in userspace.

`vcodec_mmap` also admits `0x10000000`..`0x10001000` and `0x10004000`..`0x10005000` (clock blocks, read by the
library for debugging) and a `0x07fff000`/`0x08000000` range (not needed).

## Register blocks (offsets from `0x16020000`)

| Name in the library | Base | Helper |
|---|---|---|
| MISC (also "CRC") | `+0x0000` | `H264_HAL_WriteMISC`, `…WriteCRC` |
| VLD | `+0x1000` | `H264_HAL_WriteVLD` |
| VLD_TOP | `+0x1800` | `H264_HAL_WriteVLDTOP` |
| MC | `+0x2000` | `H264_HAL_WriteMC` |
| AVC_VLD | `+0x3000` | `H264_HAL_WriteAVCVLD` |
| AVC_MV | `+0x4000` | `H264_HAL_WriteAVCMV` |
| PP | `+0x5000` | `H264_HAL_WritePP` |
| (three more, mapped, MPEG-4/VP8 side) | `+0x6800`, `+0x7000`, `+0x7800` | — |

This is the same block layout mainline Linux names `VDEC_SYS/MISC/LD/TOP/CM/AD/AV/PP` for the later MT8173
decoder (`mtk_vcodec_dec`), and the interrupt acknowledge (`0xa4`, `0x11`, `0x10`) is the same too. On MT8173
the per-picture register programming lives in a closed firmware; here it is in the open library.

## H.264: one picture, as the stock library decodes it

Function names are the library's. The chip type the library switches on is 7 for MT6582 (inferred: the mask
of supported types is 6, 7, 9, 10 and the library path names `mt6582`; branches for 6/9/10 and 8 are left out
below). `ue`/`se` are Exp-Golomb values.

**Buffers** (`Vdec_Drv_H264_open`; all physically contiguous, addresses given to the hardware are the M4U
addresses on stock; whether plain physical addresses work is the first thing bring-up answers):

| Buffer | Size | Alignment | Goes to |
|---|---|---|---|
| Bitstream | 2 MiB | 64 | `VLD+0xb0` read pointer, `VLD+0x110` write pointer, `VLD+0xb4/0xb8` start/end in 64-byte units |
| Prediction/work | 0x8000 | 0x400 | `VLD_TOP+0x28` |
| Work A | 0x1e000 | 0x800 | `MC+0xa24` |
| Work B | 0xf000 | 0x800 | `MC+0xa20` |
| Motion vectors, one per frame | 64 bytes per macroblock (`(w/16) * (h32/16) * 64`) | 0x1000 | `AVC_MV+0x20c` (current), `AVC_MV+0x180…` (references) |
| Frame | Y: `(w/16) * (h32/32) * 512`; C: half of Y, rounded up to 512 | 512 | `MC+0x224` = Y >> 9, `MC+0x228` = (Y + Ysize) >> 8 |

`h32` is the height rounded up to 32. The picture is stored in 16×32 luma blocks (the layout mainline calls
MM21: NV12 in 16×32 Y tiles and 16×16 CbCr tiles), not in raster lines.

**Per picture** (`FUN_0001afc0`), after the access unit's NAL units are in the bitstream buffer, each with a
3-byte start code, and `00 00 00 01` appended after the last one:

1. Lock (kernel: clocks on, interrupt enabled).
2. `H264_HAL_ResetHW`: wait while `VLD+0xf4` bit 15 is set until bit 0 sets (50 000 tries); `VLD+0x108 = 0x101`;
   `SYS+0x0 = 1`; `MISC+0xf4 &= ~1`; `MISC+0x84 = 2`; `MISC+0x0 = (old & 0xff400000) | 0x00801ff0`;
   `MISC+0xc8 = 0xf7`; `MISC+0xcc = 0x13a20100`; `VLD+0x108 = 0`; `SYS+0x18 = 1`; `MISC+0xec = 1`.
   Then (`InitVDecHW`) `AVC_VLD+0x280 = 0xbfdfb2f7` (error mask).
3. `H264_HAL_InitH264BarrelShift`: `VLD+0xc0 = 0x40000`; `AVC_VLD+0x84 = 0xae00e007`; `VLD_TOP+0x54 |= 1`;
   `MC+0x24 = 6`; `VLD+0xb4 = start >> 6`; `VLD+0xb8 = (start + size + 0x100) >> 6`; `VLD_TOP+0x28 = work`;
   `AVC_VLD+0x28c = 1` then `0`. Then (`…BarrelShifterAlign`) wait on `VLD+0xf4` as in step 2; `VLD+0x110 |= 1`;
   `VLD+0xb0 = start` (twice); `VLD+0x110 = end`; `AVC_VLD+0x28c = 1`; `VLD+0x108 = 0x100` then `0`;
   `VLD+0xcc = 0`; `VLD+0x8c = 0x100000`; wait until `VLD+0xe8` bit 0 is set (4096 tries; `SYS+0x0` must read
   `0x111`); `VLD+0x8c = 0x800000`; `AVC_VLD+0x28c = 0`; read `AVC_VLD+0x0`.
4. `H264_HAL_GetStartCode_8530`: read `AVC_VLD+0x0` (the next 32 bits, nothing consumed). When it is not
   `00 00 01 xx`, `AVC_VLD+0x2d8 = 1` and wait until bit 0 clears. Then consume 32 bits (start code and NAL header).
5. `parseSliceHeaderHW` walks the first slice header **through the hardware's bit reader**, which removes
   emulation-prevention bytes itself: reading `AVC_VLD + 4*n` returns the next 32 bits and consumes `n` bits
   (`n` = 0 only looks); reading `AVC_VLD+0x23c` returns and consumes one `ue`, `AVC_VLD+0x240` one `se`. Order:
   first_mb `ue`, slice_type `ue`, pps_id `ue`, colour_plane (2 bits, if separate planes), frame_num, field_pic
   and bottom_field flags (if not frame_mbs_only), idr_pic_id `ue` (IDR), pic_order_cnt_lsb and
   delta_pic_order_cnt_bottom `se`, or delta_pic_order_cnt[0..1] `se`, redundant_pic_cnt `ue`,
   direct_spatial_mv_pred (B), num_ref_idx_override and the two counts (clamped to 31). Here the picture
   registers are written (step 6), then:
   - reference list modification: `AVC_VLD+0x22c = 1`, wait until it reads non-zero (1.5 ms limit): the
     hardware reads the syntax and reorders its own lists;
   - prediction weight table (weighted P, or B with bipred idc 1): `MISC+0xf0 |= 1`, `AVC_VLD+0x230 = 1`, wait
     until it reads non-zero, then `AVC_VLD+0xf0 = (old MISC+0xf0) & ~1` (as the library does);
   - dec_ref_pic_marking (if nal_ref_idc): read flag by flag and `ue` by `ue`;
   - cabac_init_idc `ue` (CABAC, not I/SI), slice_qp_delta `se`, SP/SI fields, deblocking fields, slice group
     change cycle.
6. `Vdec_Drv_setDecPicInfo`:
   - `H264_HAL_InitPRefList`: `MC+0x564 = 0`; `AVC_VLD+0x228 = 1`, wait until bit 0 clears;
     `AVC_VLD+0xa0 = MaxFrameNum` (×2 for a field); `AVC_VLD+0xa4 = CurrPicNum`.
   - defaults: `MC+0x3dc + 4i` (i < 32) = current Y address; `AVC_MV+0x180 + 4i` = current MV buffer >> 4.
   - P list entry `i` (initial order of the standard: short-term by PicNum descending, then long-term by
     LongTermPicNum ascending), frame decoding: `MC+0x3dc + 4i` = Y address; `AVC_VLD+0xa8 + 4i` =
     `long << 19 | (FrameNumWrap or LongTermFrameIdx) & 0x7ffff`; `AVC_MV + 8i` = `buffer index << 23 |
     long << 20 | (top POC <= bottom POC) << 21 | top flags`; `AVC_MV + 8i + 4` = the same with bit 22 set and
     the bottom flags. Field flags: `0x80000` when the reference was coded as a field, `0x40000` when it was an
     MBAFF frame.
   - `H264_HAL_SetPOC`: `AVC_MV+0x200` = POC of the picture (frame: the smaller field POC), frame also
     `AVC_MV+0x204` = top POC, `+0x208` = bottom POC, each `& 0x3ffff`.
   - B lists (`MC+0x568 = 0`, `MC+0x56c = 0`; defaults `MC+0x45c + 4i` and `MC+0x4dc + 4i` = current Y):
     entry `i` of list 0: `MC+0x45c + 4i`, `AVC_VLD+0x128 + 4i`, `AVC_MV+0x80 + 8i` / `+0x84 + 8i` with the
     field POC `& 0x3ffff` added to the P format; list 1: `MC+0x4dc + 4i`, `AVC_VLD+0x1a8 + 4i`,
     `AVC_MV+0x100 + 8i` / `+0x104 + 8i`, and the co-located motion vectors `AVC_MV+0x180 + 8i` = MV buffer >> 4,
     `AVC_MV+0x184 + 8i` = that + 4 (2 and `AVC_MV+0x220 |= 0x20` with direct_8x8_inference). When list 1
     equals list 0 and has more than one entry, entries 0 and 1 of list 1 are swapped (`H264_HAL_B1ListSwap`).
   - scaling lists (`H264_HAL_WriteScalingList`), raster order, four bytes per word, first byte in bits 31..24:
     for word `k` of list `n`: `VLD+0x260 = a + 0x200`, `VLD+0x264 = word`, `VLD+0x260 = a + 0x100`, with
     `a = 16n + 4k` for the six 4×4 lists, `0x60 + 4k` and `0xa0 + 4k` for the two 8×8 lists.
   - `H264_HAL_SetSPSAVLD`: `AVC_VLD+0x88` = chroma_format_idc | `0x200000` | log2_max_frame_num_minus4 << 2 |
     poc_type << 6 | log2_max_poc_lsb_minus4 << 8 | delta_pic_order_always_zero << 12 | num_ref_frames << 13 |
     frame_mbs_only << 18 | mbaff << 19 | direct_8x8_inference << 20;
     `VLD_TOP+0x68` = (width in MBs − 1) | (height in map units − 1) << 16.
   - `H264_HAL_SetPPSAVLD`: `AVC_VLD+0x90` = cabac | pic_order_present << 1 | weighted_pred << 2 |
     weighted_bipred_idc << 3 | (pic_init_qp − 26 & 0x3f) << 5 | (chroma_qp_offset & 0x1f) << 11 |
     deblocking_control_present << 16 | constrained_intra_pred << 17 | transform_8x8 << 18 |
     (second_chroma_qp_offset & 0x1f) << 19; `AVC_VLD+0x94` = default l0 count − 1 | (default l1 count − 1) << 5 |
     per-list scaling flags in bits 10..17 | scaling enabled << 18.
   - `H264_HAL_SetSHDRAVLD1`: `AVC_VLD+0x8c` = (old & 0xfffe0000) | first_mb & 0x1fff; `AVC_VLD+0x98` =
     (old & 0x1fff) | slice_type << 13 | field_pic << 17 | bottom_field << 18 | direct_spatial << 19 |
     (l0 count − 1) << 20 | (l1 count − 1) << 25.
7. `H264_HAL_DecStart`: `MISC+0x4 = 3`; `MC+0x90 = 4`; `VLD_TOP+0x58 = ~0`; `MC+0x208 = width`;
   `MC+0x20c = height`; `AVC_VLD+0x280 = 0xbfdfb2f7`; `MC+0x220 = 1`; `MC+0x224 = Y >> 9`;
   `MC+0x228 = (Y + Ysize) >> 8`; `MC+0x250 = 1`; `MC+0x238 = 3`; `MC+0x260 = MBs wide − 1`;
   `MC+0x264 = MBs high − 1`; `MC+0x22c = MBs wide`; `MC+0x850 = (Y + Ysize*3/2) >> 4`; `MC+0x854 = Y >> 4`;
   `MC+0x858 = 1`; `MC+0x5e4 = 0x20000`; `MC+0x560 = Y`; `MC+0x55c = Ysize`; `MC+0x980 = MBs wide`;
   `AVC_VLD+0x2c8 = 0xf9040039`; `AVC_MV+0x20c = MV buffer >> 4`; `AVC_VLD+0x9c` = slice_qp_delta & 0x7f |
   disable_deblocking_idc << 7 | (alpha offset & 0xf) << 9 | (beta offset & 0xf) << 13 | nal_ref_idc << 17 |
   IDR << 19 | cabac_init_idc << 20; with CABAC `AVC_VLD+0x234 = 1`; wait until `AVC_VLD+0x25c` bit 25 clears;
   `MC+0x5e4 |= 0x1000`; `MC+0x768 = (old & ~7) | 4`; `MISC+0xf0 |= 1`; `VLD_TOP+0x6c` = macroblocks in the
   picture; `VLD_TOP+0x50 = ~0`; `MC+0xa20 = work B`; `MC+0xa24 = work A`; `AVC_VLD+0x2d0 = 0x103`;
   `AVC_VLD+0x2d4 = 0xc210`; `MISC+0x28 |= 1`; `MISC+0x3c = Y` and `MISC+0x40 = Y + Ysize*3/2` (the range the
   picture may be written to); `MISC+0x54` = lowest MV buffer address and `MISC+0x58` = end of the highest
   (same for motion vectors); `AVC_VLD+0x84 = 0xae00e007`; **`AVC_VLD+0x238 = 1` starts the decode**.
8. Wait for the interrupt (one per picture: the hardware reads the following slices of the buffer itself).
   Result: `AVC_VLD+0x284 & AVC_VLD+0x280` is the error word (0 = good), `AVC_VLD+0x278` bit 31 = picture
   complete, `MC+0x28` / `MC+0x2c` = macroblock position reached.
9. Unlock (clocks off).

The library's own DPB bookkeeping (frame-number gaps, sliding window, MMCO, output order) is ordinary H.264 and
is not copied: FFmpeg does that part for us.

**Limit seen in the library:** `H264_CheckHWParameter` and `Vdec_Drv_H264_open` refuse more than
`0x1fe000` luma samples (1920×1088) and less than 64 in either direction.

## Bring-up on the owner's player, 2026-11-12 (test builds 236 and 237)

Measured on the player, kernel `6.18.56-y2linux-1.0.0-beta.9.3.24` with `kernel/platform/vdec.c`:

- Power: after the domain sequence and the two clock-enable writes `SYS+0x0` reads `0x111` and `SYS+0x8` reads
  `0x1`; the decoder's registers answer (`MISC+0xa4 = 0x1000`, `MISC+0xc8 = MISC+0xcc = 0xffffffff` after
  power-up). Power-off and repeated cycles are clean. `CLK_CFG_1` reads `0x00810100`: the loader left the decoder's
  mux on source 1 (VENCPLL), which the display keeps running.
- Memory: the decoder reads the bitstream and writes pictures at **plain physical addresses**; the M4U is not
  needed and no IOMMU driver exists or is planned.
- The hardware's bit reader returns `0x00000165` for a buffer starting `00 00 01 65`, and `AVC_VLD+0x84` reads
  back `0x2e00c007` (bit 31 clear), so the start-code search of step 4 is skipped when the reader already stands
  on a start code. Searching anyway runs on to the *next* start code and loses the picture (the one mistake of
  the first driver version).
- Decoding: 95 pictures in six test streams (x264: intra CAVLC and CABAC at 320×240; P with one and with three
  references and weighted prediction; B pictures; 1280×720 with four slices per picture) decode with error word
  0 and are **byte-identical to FFmpeg's software decoding** after the block layout is unpacked
  (`tools/development/vdec/compare.py`). The layout is as read: luma in 16×32 blocks, CbCr in 16×16 blocks.
- These runs used the bench `vdec-lab` (the driver's `vdec-h264.c` compiled into a userspace program that
  reaches the registers through the driver's debugfs file). The kernel's own call path with the corrected
  start-code step is checked with the next kernel build.

### Through FFmpeg (test build 238, same day)

With our FFmpeg path (`buildroot/patches/ffmpeg`, hwaccel `h264_y2vdec`) and the driver's normal call:
`tools/development/vdec/vdec-ffmpeg.c` decodes each file once on the hardware and once in software and compares
every picture. Nine x264 files, 780 pictures, **none differs**: Baseline CAVLC 854×480 with three slices; Main
1280×720 with B pictures; High 1920×1080; High 1280×720 with a B pyramid, six references and weighted
prediction; High 720×576 with the JVT quantisation matrices; 426×240 (cropped); 360×640; 176×144 with 16
references; 640×480 without B pictures (picture order type 2). An interlaced 720×576 file is refused by the
hardware path at start and plays in software. One 1280×720 picture takes about 4 ms in the driver call
(`vdec-run`: 30 pictures, 4.11 ms each), a 640×368 picture 1.2 ms.

## Still open

- The chip-type value on this unit (7 assumed; the 6/9/10 branches were left out and nothing missed them).
- Long-term references and picture order type 1 have not been in a test file yet (x264 does not produce them);
  damaged streams have not been tried.
- Timing and power per picture size; sleep and wake with an open decoder.
- MPEG-4 Part 2: the sequence is in `mpeg4_part2_risc_settings` and `mpeg4_hw_reg_init`; not read out yet.
