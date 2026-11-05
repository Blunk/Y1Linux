# Y1 display: from LK's frame buffer to a Linux DRM pipeline

Status: **stage B PASS** on the owner's player (2026-11-05, [run 3](#15-stage-b-run-3--pass--2026-11-05)):
Linux's MediaTek DRM driver drives the Y1's display. Next milestones (panel re-init, backlight, power
domain, IOMMU) each need their own plan. Follows the
[charging plan](Y1-CHARGING-PLAN.md) (Linux charges the Y1; a full charge is still to be observed).

## 1. Goal

Linux drives the Y1's display through the MediaTek DRM driver instead of borrowing LK's frame buffer
(`simple-framebuffer`), so later work can change modes, blank, page-flip and eventually re-initialise
the panel. The first milestone keeps the panel exactly as LK left it: no panel reset, no init sequence,
no backlight change.

Not in scope: panel re-initialisation, the GC9503V panel variant, backlight control, the display power
domain, the IOMMU, the GPU, suspend.

## 2. Evidence

### The Y1 (recorded)

- ST7701S DSI video-mode panel, 480×360 (`lcm=0-st7701_hvga_dsi_vdo_boe`); its init table and reset
  timings (1, 20 ms, 0, 50 ms, 1, 150 ms) equal the Y2's ([Y1 hardware](../knowledge/y1-hardware.md)).
- LK leaves a lit RGB565 480×360 frame buffer at `0x9fc00000` (stride 960); `simple-framebuffer` has shown
  it on every Y1 boot; stock tagged that memory "M4U LCDC_UI".
- `clk_ignore_unused` keeps LK's display clocks on (the second boot needed it).
- Unknown: the panel reset routing, the backlight controller (MT6323 ISINK or the SoC's BLS PWM), the
  display power-domain state, LK's routing and DSI timings.

### Route A: the mt6572-mainline fork (8d427adf)

About 17 commits (March 2026), ~340 lines in 7 C files: compatibles in `mtk_drm_drv.c` (`mt6572-mmsys`
reuses the MT2701 path OVL0 → RDMA0 → COLOR0 → BLS → DSI0), `mt6572_dsi_driver_data` (cmdq `0x180`,
vm_cmd `0x130`), OVL (gmc 10 bits, 4 layers), RDMA (MT65xx RGB565 format, 3840-byte FIFO), mutex bits
(OVL 3, WDMA 6, COLOR 7, BLS 9, RDMA 10, as MT2701), MT65xx SMI common/larb (`mtk-smi.c`, +124) and an
`MTK_IOMMU_MT65XX` type in `mtk_iommu_v1.c` (+42). The diff from 6.18.56 is small in every file. Against
it: the MMSYS routing table is marked guesswork (`"??????"`, `"XXX: fixme"`), one commit forces command
mode (`"HACK … verify if we actually need this"`), there is no MT6572 PHY code (the tree reuses the MT2701
MIPI TX), no takeover of a running bootloader display beyond clearing `DSI_INTSTA`, and no commit claims a
working display. No fork board uses the ST7701S.

### Route B: the Y2's MT6582 stack

Overlays `0010`–`0018`, `0020` (~1,230 lines) and `kernel/platform` panel, MM clocks, PHY and backlight
(~1,500 lines). It works on the Y2 and holds what the fork lacks: LK takeover (backlight dark, PHY adopts
LK's live PLL without retuning, DSI video mode off, OVL and mutex reset once, LK's mutex membership
cleared, panel adopted without re-init when its reset is deasserted), the PHY bring-up order that keeps
`PLL_TOP` alive, both panel tables. Physical scan-out, no IOMMU, no SMI driver. MT6582-specific: PHY base
`0x10010000` (Y1 `0x10011000`), SMI `0x14011000` (Y1 `0x1000e000`), the DIS power sequence, the fixed
338 MHz, and Y2-only workarounds behind `innioasis,y2` / `CONFIG_Y2_PLATFORM`.

### Decision

Route A's MT6572 data as the skeleton, Route B's takeover approach and tables on top, ported as
MT65xx-generic code behind Y1 properties; the Y2 stack is not changed. No IOMMU (physical scan-out, as
the Y2 and as LK already do), no SMI or power-domain driver, `clk_ignore_unused` kept.

## 3. Stage 0: offline (no hardware)

From the 3.0.2 kernel and LK disassembly: the panel reset path (`SET_RESET_PIN` in the ST7701S LCM
driver: an MMSYS `LCM_RST_B` bit as on the Y2, or a GPIO), the backlight path (`mt65xx_leds` /
`disp_bls` / MT6323 ISINK), LK's DSI timings and PLL, and the MMSYS routing stock programs. Each becomes
an expected value for stage A.

## 4. Stage A: read-only register dump

A sixth Y1 variant (the fifth plus `DEVMEM`), with a shell command `regs` that maps fixed register windows
read-only (`/dev/mem`, `PROT_READ`) and prints them. Order: first the MMSYS clock-gate registers; a block
is read only if its clock is on (a read from a gated block can stall the bus). Windows: MMSYS config
`0x14000000` (clock gates, routing, `0x13c`), OVL `0x14007000`, RDMA `0x14008000`, BLS `0x1400a000`,
COLOR `0x1400b000`, DSI `0x1400c000`, mutex `0x1400e000`, larb0 `0x14010000`, MIPI TX `0x10011000`, the
SPM DIS power status. Interrupt status registers are read, never acknowledged. Also the MT6323 ISINK
registers through the existing `pmic` path (`0x330`–`0x356`), to find out whether LK lit the backlight
from the PMIC.

Output: LK's actual routing, mutex membership, DSI mode and timings, PHY PLL settings, the reset bit,
the backlight source, and which blocks are clocked.

## 5. Stage B: first DRM milestone (planned, admitted after stage A)

- A Y1 overlay set: the fork's MT6572 data for mmsys (routing from stage A, not the fork's guesses),
  mutex, OVL, RDMA, DSI (`mtk_drm_drv.c`, `mtk_disp_ovl.c`, `mtk_disp_rdma.c`, `mtk_dsi.c`,
  `mtk-mmsys.c`, `mtk-mutex.c`), and an MT65xx takeover modelled on the Y2's: the PHY adopts LK's PLL
  without retuning, DSI leaves and re-enters video mode at LK's timings, OVL and mutex reset once.
- A panel stub: a fixed 480×360 mode from stage A's timings; prepare/unprepare do nothing (the panel
  stays as LK set it).
- Backlight untouched. `simple-framebuffer` removed from that tree.
- PID1 shows a test pattern through DRM (dumb buffer, one mode-set), then hands back to the console.

Success: the pattern visible, the console still readable afterwards, no fault in the kernel log over the
USB shell; then reboot.

## 6. Admission for stage A (now)

| Item | Why | Bound |
| --- | --- | --- |
| **Read-only MMIO reads of the display blocks** through `/dev/mem` | §4: settles routing, timings, PHY, reset and backlight before any write | `PROT_READ` mappings of the fixed windows only; clock-gate check before each block; no interrupt acknowledged; nothing written; worst case a bus stall, which the watchdog resets within 15 s |

## 7. Admission for stage B (after stage A, not now)

| Item | Bound |
| --- | --- |
| DRM display pipeline writes (MMSYS routing, mutex, OVL, RDMA, DSI) | the blocks LK already runs; reset once; LK's timings |
| MIPI TX PHY | adopt LK's PLL, no retuning; worst case a dark screen until reboot |
| Panel | no reset, no command, no power change |
| Backlight | not touched |

## 8. Steps

1. Stage 0 offline: reset path, backlight path, LK timings and routing from the disassembly.
2. Stage A: build, owner run, `regs` over the USB shell; record.
3. Stage B plan update with measured values; owner decision on §7.
4. Stage B implementation (overlay set, takeover, panel stub, PID1 pattern); build; owner run.
5. Later milestones, each with its own plan: panel re-init (needs the reset routing), backlight, the
   display power domain, the IOMMU.

## 9. Stage A build — 2026-11-05

The sixth Y1 variant (`y1-boot.sh sixth`): `kernel/config/y1-sixth-boot.config` (the fifth fragment plus
`DEVMEM`, `IO_STRICT_DEVMEM` off), `kernel/dts/innioasis-y1-sixth-boot.dts` (the fifth tree, new model
string; charging stays available), `initramfs/y1-sixth-boot.h`: a shell command `regs` that reads SPM
`DIS_PWR_CON` (`0x23c`) and `PWR_STATUS`/`PWR_STATUS_2ND` (`0x60c`/`0x610`, display bit 3), stops if the
display domain is off, reads MMSYS `CG_CON0`/`CG_CON1` (`0x100`/`0x110`; a set bit is a gated clock,
`clk-mt6572-mm.c`), then dumps each window whose gate is clear (mmsys `0x200`, SMI common, larb0, OVL
`0x300`, RDMA, BLS, COLOR, DSI `0x200`, mutex, MIPI TX; all-zero rows skipped), and the MT6323 ISINK
block `0x330`–`0x356` through the read-only regmap file. Mappings are `PROT_READ` of one page each,
32-bit loads only.

Build `out/y1-sixth-boot-1`, BOOTIMG sha256
`311257f6881e2572ae7687445e84a3cb9ebea4c8857174889a33d95d67c02480` (`0x196000` bytes). Physical status
NOT_RUN.

## 10. Stage A result — 2026-11-05

Run on build `311257f6…`; raw output `evidence-private/2026-11-05-y1-display-a/regs-1.txt`. No stall; every
display block was clocked.

| Block | Registers | Reading (INFERRED where marked) |
| --- | --- | --- |
| Power | SPM `DIS_PWR_CON=0x0d`, `PWR_STATUS`/`_2ND=0x3fed` | display domain on |
| Clocks | MMSYS `CG_CON0=0xffdf7e00`, `CG_CON1=0xffffffec` | on: SMI common, larb0, CMDQ, COLOR, BLS, WDMA, RDMA, OVL, BLS 26 MHz, mutex slow clock, DSI engine/digital; gated: MDP, camera, codec |
| Mutex | `0x20 EN=1`, `0x2c MOD=0x688`, `0x30 SOF=1` | OVL (3), COLOR (7), BLS (9), RDMA (10), started by DSI: **the fork's mutex bits are right** |
| OVL | `0x20` ROI `0x016801e0`; L0 `0x80` addr `0x9fc00000`, `0x84` pitch `0x3c0`; L1 addr `0x9fc5a000` | LK scans out the RGB565 frame buffer by physical address (no IOMMU); a second layer exists (purpose UNKNOWN) |
| RDMA | `0x10=0x101`, width `0x1e0`, pitch `0x3c0` | enabled, direct link |
| BLS | `0x00=0x00010000`, size `0x016801e0`, `0xa0=0x3ff` | enabled in the path |
| DSI | `0x10` mode 2 (video), `0x1c` PS WC `0x5a0` (= 480 × 3: RGB888 on the link), VSA 8, VBP 100, VFP 100, VACT 360, HSA 20, HBP 230, HFP 228 (bytes) | LK's video timings |
| MIPI TX | `0x40=0x82`, `0x44=0x88492483`, `0x50=0x11`, `0x54=0x01b10007`, `0x5c=0x048b048b`, … | LK's live PLL; adopt without retuning |
| MMSYS | `0x030=1`, `0x138=0xffffffff`, `0x13c=0` | routing/reset meaning to be matched with stage 0 |
| MT6323 ISINK | `0x330`/`0x338=0x1f00`, `0x340`/`0x348=0x0f08`, `0x356=0x0033` | two sinks configured and enabled: **backlight from the PMIC, as on the Y2** (INFERRED) |

DSI `0x180`–`0x1ff` is uninitialised command-queue memory. The panel was untouched by the dump.

## 11. Stage 0 result and reconciliation — 2026-11-05

Offline, from the 3.0.2 kernel and LK (notes: `evidence-private/2026-11-05-y1-display-a/stage0-notes.md`):

- **Panel reset** is the LCD controller's `LCD_RSTB`, physical `0x14012010` (`lcm_set_reset_pin`
  0xc037dc34 → `LCD_SetResetSignal` 0xc0421f80; LK 0x8002d4f0), not a GPIO and not MMSYS. Init: reset 1,
  20 ms, 0, 50 ms, 1, 150 ms, then the 47-row table at 0xc080e564 (equal to the Y2's). Suspend sends
  0x28, 0x10, 120 ms and keeps reset high. `compare_id` reads DCS 0xA1 (ID 0x8800 or 0x9900). CONFIRMED.
- **Backlight:** `lcd-backlight` = MT6323 PMIC mode, ISINK0 and ISINK1 together (`brightness_set_pmic`);
  the panel has no `set_backlight`; BLS PWM and `Cust_SetBacklight` are unused for the LCD. CONFIRMED.
  Stage A's ISINK registers match: both enabled (`0x356` bits 0/1), duty index 31 (`0x330`/`0x338` bits
  12:8), double-enable `0x350`, phase `0x354`, chop `0x356` bits 4/5, `fsel` 2 (`0x332`/`0x33a`); the
  live step field is 4 (`0x334`/`0x33c` bits 14:12), not the table's 1 (to be resolved before any
  backlight write).
- **DSI:** sync-pulse video, 2 lanes, RGB888, 480 × 360; pixels VSA 8, VBP 100, VFP 100, HSA 10, HBP 80,
  HFP 80; PLL `div1` 2, `div2` 0, feedback 15. Stage A's byte-unit horizontal values (20/230/228) are these
  pixel values after the DSI driver's conversion, the vertical ones identical, the MIPI TX `0x50`/`0x58`
  fields equal. ~~INFERRED rate: 26 MHz × 60 / 4 = 390 Mbit/s per lane, about 88 Hz at 650 × 568.~~
  **Corrected (§12):** mainline's MIPI TX formula (`pcw = rate × 2 × txdiv / 26 MHz`) reproduces both
  LK fields, TXDIV0 2 and PCW 60, only at **195 Mbit/s per lane**: a 16.25 MHz pixel clock, about 44 Hz.
- **Routing and mutex:** `disp_path_config_` writes MMSYS `0x030` OVL_MOUT_EN 1, `0x04c`–`0x058` 0, mutex
  MOD `0x688`, SOF 1, EN 1 after a reset pulse; stage A reads exactly these. Path OVL → RDMA0 → COLOR →
  BLS → DSI, mutex bits as the fork's MT2701 layout. LK also programs COLOR (`0x400`, `0xf00`, `0xf50`/`0xf54`).

Every value stage B needs is now measured and matches the stock code.

## 12. Stage B build — 2026-11-05

Owner approval of §7: 2026-11-05.

- **Overlays `0018`–`0023`**, the fork's MT6572 data backported to 6.18.56 (fork commits 861904e64464,
  3f0382b895cb, 0468199b9247, 81c88d334c74, 16573da53f4f, 6631961b8cbc, 768f85b0f607, 224444cb531a and the
  `mtk-mmsys.c` part of 603809c4357e): DRM compatibles (`mt6572-mmsys` uses the MT2701 path OVL0 → RDMA0 →
  COLOR0 → BLS → DSI0), `mt6572_dsi_driver_data` and the `DSI_INTSTA` clear before the IRQ, OVL (gmc 10,
  4 layers), RDMA (MT65xx formats, 3840-byte FIFO, no size writes), mutex bits (OVL 3, COLOR 7, BLS 9,
  RDMA 10, WDMA 6), MMSYS driver data. Only hunk conflict: the 6.18 DSI match table's extra `mt8167` line.
  Not taken: SMI, IOMMU, the forced command mode (973e5b4ededb), underflow logging.
- **MMSYS routing** (`kernel/y1/mt6572-mmsys.h`, added as `drivers/soc/mediatek/mt6572-mmsys.h`): the
  stock/LK values of §11 instead of the fork's guessed table.
- **Panel stub** (`kernel/y1/panel-lk.c`, overlays `0024`/`0025`, `DRM_PANEL_INNIOASIS_Y1_LK`): LK's mode,
  2 lanes, RGB888, sync-pulse video; prepare/enable/disable/unprepare do nothing.
- **No PHY patch:** for 195 Mbit/s mainline's MT8173-style MIPI TX code writes LK's TXDIV0 and PCW, and its
  enable order (bandgap, LDO/core enable, SDM power, PLL_EN, PRESERVE `0x300`, lane enables) matches the
  stock sequence; the PLL is briefly switched off and on during the modeset.
- **Tree** `innioasis-y1-seventh-boot.dts`: the sixth tree without `simple-framebuffer`, DSI and BLS
  enabled (BLS as a DRM pass-through; `PWM_MTK_DISP` not built), `iommus` deleted from OVL and RDMA, the
  panel stub as `dsi`'s `panel@0`. **Config** `y1-seventh-boot.config`: `DRM`, `DRM_FBDEV_EMULATION`,
  `MTK_MMSYS`, `DRM_MEDIATEK`, `PHY_MTK_MIPI_DSI`, the panel stub; no IOMMU or SMI. `y1_boot.py` now keeps
  the frame-buffer lines per variant and refuses `simple-framebuffer` or `iommus` in the seventh tree.
- **PID1** (`initramfs/y1-seventh-boot.h`): after the storage and USB lines, eight colour bars on
  `/dev/fb0` for 10 s (the screen saved and restored), and a shell command `bars`.

Build `out/y1-seventh-boot-1`, BOOTIMG sha256
`e58456a95103d767ee67ac096e78ae358b448cc2f927f3fa887f8affbb50096b` (`0x1dc000` bytes), no compiler
warnings. Physical status NOT_RUN.

## 13. Stage B run 1 and correction — 2026-11-05

Run 1 (`e58456a9…`): the screen kept LK's boot logo; Linux ran (USB shell up). Kernel log over the shell
(`evidence-private/2026-11-05-y1-display-b/run1-dmesg.txt`): the MIPI TX PHY and the MMSYS/DRM drivers
probed and matched OVL, RDMA, COLOR and DSI, but `panel-innioasis-y1-lk 1400c000.dsi.0: error -19: DSI
attach`. 6.18's `mtk_dsi_host_attach` finds its panel only through an OF graph
(`devm_drm_of_get_bridge` → `drm_of_find_panel_or_bridge`, `-ENODEV` without `of_graph_is_present`), so
the DSI component never registered, the DRM master never bound, and no display register was written:
the logo stayed because nothing replaced LK's scan-out. Correction: a `port`/`endpoint` pair between
`dsi` and `panel@0` in the seventh tree; `y1_boot.py` requires it.

Build `out/y1-seventh-boot-2`, BOOTIMG sha256
`1cad009c8ed173f56500cbdce538547a55e413e6af9e94fad979389f3e6b4199`. Physical status NOT_RUN.

## 14. Stage B run 2 — the DRM pipeline drives the display — 2026-11-05

Run 2 (`1cad009c…`; `evidence-private/2026-11-05-y1-display-b/run2-*`): every component bound
(`bound 14007000.ovl`, `…rdma`, `…color`, `…dsi`), `[drm] Initialized mediatek 1.0.0`, `Console:
switching to colour frame buffer device 60x45`, `fb0: mediatekdrmfb`, the panel stub adopted LK's panel;
no error (expected notices: the DSI↔panel graph cycle, the unused second CRTC). DRM state: `crtc-0
enable=1 active=1`, plane 0 scanning fbcon's 480×360 XRGB8888 buffer. Registers after takeover (read-only
`regs`): OVL layer 0 enabled alone (`0x2c=1`), address `0x80c00000` (the DRM buffer), pitch 1920; LK's
layers 2/3 off. Mutex and MMSYS routing as LK (`MOD=0x688`, `SOF=1`, `0x030=1`). MIPI TX as LK except
`0x54` bit 2 (spread spectrum, cleared by mainline). DSI: mainline's horizontal blanking differs a little
from LK's (HBP 208 vs 230, HFP 217 vs 228 bytes, the line about 2 % shorter).

The colour bars did not run: `allnoconfig` leaves `CONFIG_FB_DEVICE` off, so 6.18 created `/dev/dri` but no
`/dev/fb0`, and PID1's failure message went to the screen only. Build `out/y1-seventh-boot-3` adds
`FB_DEVICE` and sends the pattern's messages to the USB shell too; BOOTIMG sha256
`a28795f2c16aebd879a4407706ed980f53a21cafe477cf4bb31b47fec75c989f`. The owner: in run 2 the screen showed
Linux console text (the takeover worked; only the pattern was missing).

## 15. Stage B run 3 — PASS — 2026-11-05

Run 3 (`a28795f2…`; `evidence-private/2026-11-05-y1-display-b/run3-dmesg.txt`): the owner saw Linux
console text on the panel and the colour bars at boot; over the USB shell `bars` drew them again
(`DRM frame buffer 480x360 32 bpp, stride 1920`, `colour bars shown for 10 s, screen restored`). Kernel log:
`[drm] Initialized mediatek`, `fb0: mediatekdrmfb`, no error or underflow.

**Stage B: PASS.** The Y1's display runs on Linux's own pipeline (OVL → RDMA → COLOR → BLS → DSI, MIPI TX)
from a Linux-allocated XRGB8888 buffer, with the panel and backlight as LK left them. Not covered: panel
power-down/re-init (reset is `LCD_RSTB` `0x14012010`), backlight control (MT6323 ISINK0/1), blanking, the
display power domain, the IOMMU, LK's exact horizontal blanking (mainline's is about 2 % shorter and the
panel accepts it).
