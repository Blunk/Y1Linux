# Y1 second boot: MT6572 base drivers, read-only storage and input

Status: approved by the owner with all three admission items (2026-11-04); steps 1–3 done and built
offline, physical run NOT_RUN ([build](#8-build-result--2026-11-04)). Follows the
[first boot](../knowledge/y1-hardware.md#first-boot-result--2026-11-04) (Linux 6.18 reached PID1 on the
owner's Y1).

## 1. Goal

A second test kernel that proves the foundation every later Y1 feature needs, while writing nothing to the
player's storage:

1. The MT6572 clock, pin and PMIC-wrapper drivers run on the Y1 without disturbing what LK left running
   (the screen stays lit).
2. The eMMC and the microSD slot are detected and read, **read-only**: the screen shows the devices, their
   sizes, and SHA-256 digests of known regions that can be compared with the 2026-11-04 backup. The eMMC
   boot areas (BOOT1/BOOT2), which mtkclient could not read, get read here for the first time.
3. The wheel (APT32F on I2C1) and the centre button (MT6323 power key) deliver key events, shown on screen.
4. The serial console's driver binds (the first boot's `-ENOENT`).

Not in scope: display driver, audio, charging, suspend, USB, Wi-Fi/Bluetooth, the amplifier, any write to
the eMMC or the SD card, Buildroot.

## 2. Sources

From [mt6572-mainline/linux](https://github.com/mt6572-mainline/linux) `master` (69 commits on mainline
after v6.18; its patches are written against that newer mainline and are **backported** to our 6.18.56).
Only what the goals need:

| Area | Fork files | Size |
| --- | --- | --- |
| Clocks | `drivers/clk/mediatek/clk-mt6572-{topckgen,apmixedsys,infracfg}.c` (mm, mfg not needed), Kconfig, Makefile | ~570 lines |
| Pins and EINT | `drivers/pinctrl/mediatek/pinctrl-mt6572.c`, `pinctrl-mtk-mt6572.h` (paris, with `mt6572_eint_hw`), Kconfig, Makefile | ~1,770 lines, mostly pin tables |
| PMIC wrapper | `drivers/soc/mediatek/mtk-pmic-wrap.c` (MT6572 data) | +116 |
| I2C | `drivers/i2c/busses/i2c-mt65xx.c` (MT6572, separate RX DMA channel, WRRD fix) | +59/−11 |
| Watchdog resets | `drivers/watchdog/mtk_wdt.c` (MT6572 TOPRGU reset data, used by the PMIC wrapper) | +6 |
| Bindings | `include/dt-bindings/{clock/mediatek,mt6572-clk.h, reset/mediatek,mt6572-resets.h, pinctrl/mt6572-pinfunc.h}` | ~990 lines |
| SoC description | `arch/arm/boot/dts/mediatek/mt6572.dtsi` (the fork's, with clock, pin, PMIC-wrapper, I2C, MMC nodes) | 628 lines |

Already in mainline 6.18: `mtk-sd` (the fork uses `mediatek,mt2701-mmc`), the MT6323 MFD, `mtk-pmic-keys`
(`mediatek,mt6323-keys`), `mt6323-regulator`. Not taken: power domains, SMI/IOMMU, display, GPU, thermal,
cpufreq, hotplug, sensors, panels. Each ported file keeps its SPDX line and copyright; the commit message
names the fork commit it came from.

The wheel driver is ours: `kernel/platform/apt32f-wheel.c` (Y2). The Y1 sends its buttons in the same frame
(class 1 press / class 2 release, register 3 a bitmask; [evidence](../knowledge/y1-hardware.md)), which the
Y2 driver ignores. It gains that handling behind a device-tree property, so the Y2's behaviour and output
stay as they are.

## 3. Build tooling

1. **A Y1 overlay set separate from the Y2's.** `run.py` takes a manifest argument; the Y1 build uses
   `kernel/patches-y1/manifest.json` and none of the 53 Y2 overlays. Three files both need
   (`i2c-mt65xx.c`, `mtk_wdt.c`, the pinctrl Kconfig) then never carry each other's changes, and the Y2's
   build inputs stay byte-identical.
2. **Added files.** The overlay format only replaces existing upstream files; the port adds ~15 new ones in
   existing directories. The manifest gains `added` entries (path, source, sha256), bound read-only into
   `/src/<path>` the way `run.py` already lays out `/src/drivers`.
3. **The wheel driver** builds for the Y1 from the same source file (no copy), under its own Kconfig symbol.
4. Tests: manifest and added-file hashing (as `tests/test_overlay_hashes.py`), the build's offline checks.

## 4. Device tree and kernel

`kernel/dts/innioasis-y1-second-boot.dts` on the fork's `mt6572.dtsi`; the first-boot tree stays as it is.

| Node | Setting |
| --- | --- |
| Command line | adds `clk_ignore_unused`: without it the clock driver turns off the display clocks LK left on and the screen goes dark |
| UART0 | the fork's `baud` + `bus` clocks |
| eMMC (`mmc0`) | 8-bit, `cap-mmc-highspeed` only, `max-frequency` 50 MHz (no HS200/DDR), `non-removable`, no supply nodes (LK leaves them on), an **empty** pinctrl state (keeps the preloader's pin setup) |
| microSD (`mmc1`) | 4-bit, `cap-sd-highspeed`, `broken-cd` (detect pin unknown), no supplies, empty pinctrl state |
| I2C1 | `wheel@51`: reset GPIO134 active low, interrupt EINT2 (GPIO136), supply MT6323 VGP1 3.0 V (§6) |
| I2C0 | disabled (the amplifier stays untouched) |
| PMIC wrapper | MT6323 with only its `keys` child (power key = centre button) and the one `vgp1` regulator (§6); no other regulator is described, so Linux cannot switch any other off |
| Not described | power domains, display, USB, audio, battery, charger |

Kernel config: the first-boot fragment plus the clock, pinctrl (paris, EINT), PMIC-wrapper, MFD, PMIC-keys,
I2C, MMC + `MMC_BLOCK`, `EVDEV` and the wheel driver. **No filesystem** is built in (nothing can be
mounted), `BLOCK` only for the read-only test.

## 5. Test PID1

`initramfs/y1-init.c` grows a second mode (the first boot stays reproducible):
1. Watchdog as before; banner.
2. As soon as each `mmcblk*` node exists: `BLKROSET` (read-only) before any read.
3. Shows `/proc/partitions` and SHA-256 of: eMMC user area `0x0`–`0x200000` (expected `f3748d6a…517713`
   from the backup), the PMT region and `BOOTIMG` (expected: the image then installed), `mmcblk0boot0`/`boot1`
   whole (new: no reference exists yet), and the first MiB of the SD card if present.
4. Shows every key event from `/dev/input/event*` for 60 s (code and name), heartbeat, then reboots.

A digest that matches the backup proves the driver reads correctly; one that does not is a finding, not a
write. SHA-256 is a small freestanding implementation, checked under QEMU against known vectors at build.

## 6. Safety and admission

Owner decisions needed before implementation (the first-boot admission excluded these):

| Item | Why | Bound |
| --- | --- | --- |
| eMMC and SD **read** from the test kernel | goal 2 | no filesystem driver; `BLKROSET` before reading; conservative bus modes; the MMC core's own commands only (bus width, speed, power-off notice) |
| PMIC wrapper and MT6323 access | centre button | the keys child only; wrapper initialisation is skipped when LK already did it (it has) |
| Enable **VGP1 at 3.0 V** | the wheel controller's supply (stock: `hwPowerOn(5, 3000)`) | that one regulator, the stock voltage; nothing else described |
| Pin and clock drivers active | everything above | `clk_ignore_unused`; empty pin states for storage; only the wheel's two GPIOs requested |

Failure modes: the screen goes dark (a clock or power domain turned off: the watchdog still reboots in
120 s); a driver hangs (watchdog reset within 15 s once PID1 runs); a storage digest mismatch (recorded,
nothing written). Recovery is unchanged: BOOTIMG through the preloader.

## 7. Steps

1. Tooling: Y1 manifest, added files, tests.
2. Backport and review the fork files against 6.18.56; build them with the first-boot tree (no behaviour
   change expected) to catch API differences first.
3. Second-boot tree, config and PID1; offline checks (config, device tree, image, reproducibility).
4. Owner run: write, read back, boot, photograph, restore (as the first boot).
5. Record the result; audit entry; next boundary (display driver or storage writes).

## 8. Build result — 2026-11-04

`python3 tools/build/run.py --manifest kernel/patches-y1/manifest.json --output out/<fresh> -- sh
/project/tools/build/y1-boot.sh second`; two fresh builds byte-identical. BOOTIMG 1,511,424 bytes, sha256
`0110551f2e8b80c73b94aaba7aee1186780843b466cafca9378328e7e8579575`; zImage+DTB `1469e60d…1920`, DTB
`e6c83868…f3fa`, PID1 `633b9d19…ded4da`, config `5d09e9e4…1877`. No compiler warning.

What went in, against the plan:
- `kernel/patches-y1`: the fork's changes to eight upstream files carried from 7.0-rc7 to 6.18.56 (one I2C
  hunk not needed: 6.18 already has the fixed WRRD condition), the wheel driver's Kconfig/Makefile entries,
  and the Y2's reviewed `ARM_VIRT_EXT` visibility patch unchanged (D09; the first boot ran with `n` too).
  `kernel/mt6572`: twelve new fork files; `pinctrl-mt6572.c` takes 6.18's include order (the only edit).
- `kernel/dts/mt6572-fork.dtsi` (the fork's SoC file, unchanged) and `innioasis-y1-second-boot.dts`. The
  PMIC is not upstream `mt6323.dtsi`, which describes every regulator; the Y1 stock kernel confirms the PMIC
  interrupt (EINT16, level high) and the wheel's pin set-up (GPIO136 mode 4, pull-up, falling edge).
- `kernel/platform/apt32f-wheel.c`: zones in the frame behind `linux,keycodes`, the stock power-up behind a
  `vdd` supply; neither is in the Y2 tree, whose behaviour is unchanged.
- `initramfs/y1-init.c` + `y1-second-boot.h` (`-DY1_SECOND_BOOT`): SHA-256 checked under QEMU; the first-boot
  image rebuilds byte-identical (`12861e2a…`) with the shared, now variant-taking `y1-boot.sh`/`y1_boot.py`.
- `run.py`: `--manifest` and `added` files; `tests/test_overlay_hashes.py` covers both overlay sets.

Offline checks: no filesystem configured; the packaging requires the expected device-tree properties,
exactly four enabled devices (UART0, I2C1, both MMC hosts) and no regulator but VGP1.

## 9. Run 1 and correction — 2026-11-04

Run 1 (`0110551f…`, the owner's photo in `evidence-private/2026-11-04-y1-second-boot/`): the kernel booted
and PID1 reached the storage step, but no disk appeared (`/dev/mmcblk0`, `mmcblk0boot0`, `mmcblk0boot1`:
not present), so nothing was read. The SD host looped on `cmd=8 … host->error=0x2`, `no support for card's
volts`, `error -22 whilst initialising SD card`. Cause: 6.18 `mtk-sd` sets `ocr_avail` only from a `vmmc`
supply, and the tree gave none, so the MMC core rejected every card voltage (`mmc_select_voltage`).
`broken-cd` polling repeated it every second and filled the screen.

Correction (build `out/y1-second-boot-4`, reproducible): both hosts get a `regulator-fixed` 3.3 V
`regulator-always-on` supply, a description of the rail LK leaves on (stock VEMC_3V3, 3.3 V) that switches
nothing; `CONFIG_REGULATOR_FIXED_VOLTAGE=y`; the SD host is `non-removable` (probed once at boot). The
packaging admits exactly these two fixed supplies besides VGP1. BOOTIMG 1,513,472 bytes, sha256
`7ea60d9abfb86e50a64bceaf823c129b6c0e259952e1e5f0967f81acc771c465`. Physical status NOT_RUN.

## 10. Run 2 and corrections — 2026-11-04

Run 2 (`7ea60d9a…`, photo `evidence-private/2026-11-04-y1-second-boot/run2-storage.jpg`): **storage works.**
`mmcblk0` 3696 MiB (= the DA's `0xe7000000`), `mmcblk0boot0`/`boot1` 2048 KiB each, the owner's microSD
`mmcblk1` with `mmcblk1p1`; no read error. The three user-area digests differed from the backup, and the
boot-area digests matched backup bytes 0–2 MiB and 2–4 MiB: the mtkclient dump is MediaTek's logical space
(BOOT0, BOOT1, then the physical user area from `0x1400000`), so the expected values were 20 MiB off. The
ARM digest routine reproduces the backup's values under QEMU; LOGO's on-screen value equals the dump's zero
region 20 MiB later. Corrected in [Y1 hardware evidence](../knowledge/y1-hardware.md) and the backup manifest.

The wheel registered (`input: APT32F click-wheel … 1-0051`), then failed: `mt6572-pinctrl: Can not find
pin`, `request_irq … -22`. The fork's pin table maps an EINT for only 2 of 154 pins; GPIO136 now carries
`MTK_EINT_FUNCTION(4, 2)` (its `FUNC_EINT2` mode), on that pin only.

Build `out/y1-second-boot-6` (reproducible): PID1 digests at physical offsets of partitions stock Android does
not rewrite (MBR+EBR1, UBOOT, RECOVERY, LOGO) and the boot areas against dump 0–4 MiB. BOOTIMG sha256
`81f9729113997f3af1f9fce0db784e736d282b7b40ed57c33a1a500c3d1c4730`. Physical status NOT_RUN.

## 11. Run 3 — 2026-11-04

Run 3 (`81f97291…`, photo `run3-storage.jpg`): `mmcblk0boot0` and `mmcblk0boot1` **match** the backup's
first 4 MiB; MBR+EBR1, UBOOT, RECOVERY and LOGO at "physical = logical − 0x1400000" still differ, so that
shift holds for the boot areas only and the user area's mapping is open. No 1 MiB-aligned dump window
reproduces the on-screen LOGO prefix. The wheel's EINT now resolves; the request fails one step later:
`gpiochip_lock_as_irq: tried to flag a GPIO set as output for IRQ`, `request_irq … -5`: GPIO136 is an
output, and on paris `input-enable` sets only the input buffer.

Build `out/y1-second-boot-8` (reproducible), BOOTIMG sha256
`3df3443d08ae5274dfcf1aeaa09d4d19c7870760b0572d54e1e7709f57a46641`: the wheel pin adds
`input-schmitt-enable` (paris sets the direction to input with it); PID1 lists the physical offsets of
`ANDROID!` headers and `55AA` tables in the first 64 MiB (the dump has them at logical `0x3180000`,
`0x3780000` and `0x1400000`, `0x1480000`). Physical status NOT_RUN.

Centre button (owner, 2026-11-05, earlier runs): pressing it prints `key 28 ENTER (centre)`, so the
PMIC wrapper, the MT6323 MFD with its EINT16 interrupt and `mtk-pmic-keys` work on 6.18; goal 3 is met
for the centre button, the wheel remains open.

## 12. Run 4 — 2026-11-05

Run 4 (`3df3443d…`, photos `run4-storage.jpg`, `run4-keys.jpg`):

- **Wheel and all buttons work.** The APT32F wheel probes on I2C1 with VGP1 at 3.0 V and its EINT2
  interrupt (`firmware 1`); the button test shows wheel scroll as `KEY_UP`/`KEY_DOWN`, the four buttons
  as `KEY_BACK` (top), `KEY_PLAYPAUSE` (bottom), `KEY_NEXTSONG` (right), `KEY_PREVIOUSSONG` (left), each
  with down and up, and the centre button as `KEY_ENTER`. Goal 3 is met.
- **User-area mapping measured.** `55AA` at physical `0xe00000`, `0xe80000` and `ANDROID!` at
  `0x2b80000` (the test BOOTIMG), `0x3180000` (RECOVERY), against dump `0x1400000`, `0x1480000`,
  `0x3780000`: physical = logical − `0x600000`; the dump's first 6 MiB are BOOT0, BOOT1 and RPMB.
  The run-4 digests at the old offsets equal the dump at +6 MiB (MBR window `30e14955…`, UBOOT window
  `a6619f48…`, RECOVERY window `1f56c414…`; the LOGO window overlaps the rewritten BOOTIMG).
  The expected digests are unchanged and hold at physical `0xe00000`, `0x2b20000`, `0x3180000`,
  `0x3840000`, which PID1 now uses.

Build `out/y1-second-boot-10`, BOOTIMG sha256 `23e011b0bb15d9a9b73ba7af235ad8ddd3cadd3e8ca6cf194e971f33b02f04d0` (`0x171800` bytes),
corrects only those four offsets. Physical status NOT_RUN; a run should
show four `MATCHES backup` lines, after which goal 2 is met.

## 13. Run 5 — second boot PASS — 2026-11-05

Run 5 (`23e011b0…`, photo `run5-storage.jpg`): MBR+EBR1 (`2b036716…`), UBOOT (`66c92662…`), RECOVERY
(`e9fe26a2…`), LOGO (`ef315556…`), `mmcblk0boot0` and `mmcblk0boot1` all print `MATCHES backup`; the
marker line is unchanged from run 4. With run 4's button test, every goal of §1 is met on the device:
clocks, pins and the PMIC wrapper bind on 6.18; Linux reads the eMMC (user area and both boot areas)
and the microSD read-only and agrees with the backup; the wheel, its four buttons and the centre button
deliver key events. **Second boot: PASS.** No storage was written by Linux; stock was restored between runs
by writing `boot-stock-from-player.img` to BOOTIMG.
Not covered: charging (no charger or battery driver; the owner charges under stock), display driver,
audio, USB.
