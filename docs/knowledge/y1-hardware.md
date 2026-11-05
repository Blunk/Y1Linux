# Innioasis Y1 (MT6572) hardware evidence

<!-- knowledge-base-scope: retained-research-/-scoped-evidence -->
> **Historical record.** The dates, candidate identity, "current" claims,
> next steps and permissions below belong to this recorded boundary. See
> [current state](../CURRENT_PLATFORM_STATE.md) for the latest physically observed result.

Date: 2026-11-04. Scope: a possible Y1 board next to the Y2, recorded for the
[Y1 port scope audit](../planning/roadmap-gap-audit.md#y1-mt6572-port-scope-audit--2026-11-04).
Offline analysis of a public stock ROM plus owner observation only: no ADB, no boot-ROM
session, no flash, no device read. Nothing here is Y1 implementation or hardware authorization.

Vocabulary as in [M0](../planning/M0-evidence-and-recovery.md): CONFIRMED applies to the named
artifact; a fact about the owner's player stays INFERRED until read on that player.

## Owner's player

| Fact | Status |
| --- | --- |
| Physical Innioasis Y1, stock firmware **3.0.2**, wheel works | CONFIRMED by the owner, 2026-11-04 |
| **Type A** Y1: the `type-b-1.7.6` release exists for Y1s whose wheel stops working on firmware 2.0.0 or newer | INFERRED from the working wheel on 3.0.2 |
| ADB off on stock | CONFIRMED by the owner (no ADB device); explained by the ramdisk below |

## Read-only backup of the owner's player — 2026-11-04

The owner ran mtkclient v2.1.4.1 (`5a863eec`) read commands only (`printgpt`, `rf`, `ro`); mtkclient connected
through the preloader and loaded its bundled legacy DA (`MTK_AllInOne_DA_mt6590.bin`, `49a14137…`) into RAM.
Manifest and images: `evidence-private/2026-11-04-y1-backup/` (not published).

| Fact | Status |
| --- | --- |
| MT6572, hw code `0x6572`, subcode `0x8b00`; SBC, SLA and DAA off | CONFIRMED on the device |
| DRAM `0x20000000` (512 MiB) | CONFIRMED (DA report) |
| eMMC user area `0xe7000000` (3.61 GiB); boot1, boot2 and RPMB 2 MiB each | CONFIRMED |
| Partition table equals the 3.0.2 scatter; `FAT` is `0x18bc0000` (scatter: 0) | CONFIRMED |
| ~~Dump offsets equal table offsets: the user area starts with an `EMMC_BOOT` header and the preloader at `0x800`~~ ~~physical = logical − `0x1400000`~~ **Measured by the second boot (run 4):** the dump is MediaTek's *logical* space: BOOT0 (`EMMC_BOOT` header, preloader at `0x800`) at 0–2 MiB, BOOT1 at 2–4 MiB, RPMB at 4–6 MiB, then the user area: **physical = logical − `0x600000`**. Linux finds the MBR/EBR1 `55AA` at physical `0xe00000`/`0xe80000` (dump `0x1400000`/`0x1480000`) and RECOVERY's `ANDROID!` at `0x3180000` (dump `0x3780000`); `mmcblk0boot0`/`boot1` equal dump 0–2/2–4 MiB. The dump lacks the last 6 MiB of the user area, past `FAT` | CONFIRMED (Linux reads, 2026-11-05) |
| Preloader, MBR, EBR1, LK, BOOTIMG and RECOVERY byte-identical to the public 3.0.2 ROM, so the kernel analysis applies to this player | CONFIRMED |
| Whole user area read once (sha256 `796cdf27…3497ef`); `0x0`–`0x3120000` (through SECCFG, including NVRAM and PROTECT_F/S) read twice, identical | CONFIRMED |
| eMMC BOOT1/BOOT2: both `--parttype` reads returned dump 0–2 MiB (the partition switch is unchecked). **Corrected:** BOOT0 and BOOT1 are *in* the user-area dump (0–2 MiB and 2–4 MiB) | ACQUIRED, confirmed by Linux digests |
| Any restore; an independent second copy | NOT PROVEN / owed |

## ADB evidence boot — 2026-11-04

With the owner's go-ahead, BOOTIMG on the player was replaced by an evidence image: the player's own boot
image with only the ramdisk's `default.prop` (`ro.secure=0`, `ro.debuggable=1`, `ro.adb.secure=0`,
USB `mass_storage,adb`) and an appended `init.rc` block (USB mode at boot completion; a root `dmesg` copy to
`/data/local/tmp` at `post-fs-data`). Kernel and the rest of the header byte-identical; read back after the
write, identical (sha256 `6011b72d…be4057`). The player boots normally with it; it is still installed.
Restore: write `boot-stock-from-player.img` (`121bad1a…`) to BOOTIMG. Image, tool, runbook and captures:
`evidence-private/2026-11-04-y1-adb-boot/` and `…-y1-adb-capture/`.

`adbd` still runs as `shell` (uid 2000), so `dmesg` and `/proc/cmdline` are refused live; the early root copy
starts at 0.36 s because the log buffer had already wrapped, so `mt_fixup`'s `[Phy Layout]` lines are lost.
CONFIRMED on the owner's player:

| Fact | Evidence |
| --- | --- |
| Command line `console=ttyMT0,921600n1 vmalloc=500M slub_max_order=0 lcm=0-st7701_hvga_dsi_vdo_boe fps=3643 … printk.disable_uart=1` | `mtkfb_probe` log line |
| Panel **ST7701S** (the same as the owner's Y2) | `lcm=0-st7701_hvga_dsi_vdo_boe` |
| Console **UART0** `mtk-uart.0` at `0x11005000` (UART1 at `0x11006000`); this corrects the offline reading of LK's default string | command line, `/proc/iomem` |
| RAM: `0x80100000`–`0x9dffffff` and `0x9ea00000`–`0x9fbfffff` System RAM; `0x9e000000`–`0x9e9fffff` (10 MiB) held back, purpose UNKNOWN; `0x9fc00000`–`0x9fffffff` frame buffer (4 MiB, M4U LCDC_UI); `MemTotal` 494,388 kB | `/proc/iomem`, `/proc/meminfo`, M4U log |
| I2C0 `0x58` `aw87559_pa`; I2C1 `0x51` `APT32F`, `0x60` `UPDATE`; leftovers `pn544`, `dummy_eeprom`, `dummy_cam_cal` registered | `/sys/bus/i2c/devices` |
| I2C0 `0x11009000`, I2C1 `0x1100a000`, MSDC0 `0x11120000`, MSDC1 `0x11130000`, Mali `0x13010000` | `/proc/iomem` |
| Wheel power: PMIC powerId 5 set to 3000 mV and enabled | `pmic_ldo_vol_sel` log |
| No ALSA card (stock audio via `/dev/eac`); frame buffer 480×1080 virtual, 32 bpp | `/proc/asound`, `fb0` |
| Inputs: `mtk-kpd` event0, `ACCDET` event1, `mtk-tpd` event2, `mtk-tpd-kpd` event3 | `/proc/bus/input/devices` |

Buttons (owner pressed each once with `getevent -lt` running; the Y1 has no volume and no power buttons):

| Control | Event |
| --- | --- |
| Wheel clockwise / counter-clockwise | event2 `KEY_LEFT` / `KEY_RIGHT`, press and release together per step |
| Top, bottom, left, right | event2 `KEY_BACK`, `KEY_PLAYPAUSE`, `KEY_PREVIOUSSONG`, `KEY_NEXTSONG`, separate press and release |
| **Centre** | event0 (`mtk-kpd`) `KEY_REPLY` (232): the MT6323 power-key input, reported with the handler's alternate code |

## First-boot test design — 2026-11-04

Admitted by the owner as source and offline work ([admission](../planning/roadmap-gap-audit.md#y1-first-device-tree-and-minimal-boot-test-admission--2026-11-04)).
Build: `python3 tools/build/run.py --output out/<fresh> -- sh /project/tools/build/y1-boot.sh first` (the script was `y1-first-boot.sh` when this image was built; same commands).

| Part | Choice | Why |
| --- | --- | --- |
| Device tree | `kernel/dts/innioasis-y1-first-boot.dts` on upstream v6.18 `mt6572.dtsi`: CPU0 (SMP off), MTK timer, watchdog, UART0 at 921,600, `simple-framebuffer` at `0x9fc00000` | Mainline 6.18 already describes these; no SoC patch |
| Memory | `0x80000000`–`0x9dffffff` and `0x9ea00000`–`0x9fbfffff` | Stock System RAM plus the idle connsys MiB at the base (the kernel lives there with `AUTO_ZRELADDR`, as on the Y2); the unexplained 10 MiB and the frame buffer stay out |
| Kernel | `kernel/config/y1-first-boot.config`: the Y2's booted first-boot config without its diagnostic, plus fbcon on `simplefb`, `MEDIATEK_WATCHDOG` and `WATCHDOG_HANDLE_BOOT_ENABLED` (allnoconfig turns that default off); Y2 options off, so the Y2 overlays stay inert | The screen is the only output the owner can see; `mtk_wdt` adopts a watchdog LK left running (15 s, `mt6572.dtsi`) and the core feeds it until PID1 takes over |
| Userspace | `initramfs/y1-init.c` built into the kernel (`initramfs/y1-first-boot.list`); opens `/dev/watchdog` (starting it if LK left it stopped) and feeds it every heartbeat; prints a banner, the kernel version and `MemTotal`, a heartbeat every 5 s for 120 s, then reboots | Independent of where Y1's LK puts the ramdisk; a hang after PID1 starts ends in a watchdog reset; the reboot gives the owner a preloader window to restore BOOTIMG (no power button) |
| Image | `tools/build/y1_boot.py` (then `y1_first_boot.py`): zImage + DTB, an empty ramdisk, the shared `bootimage.pack()` (the Y1 stock header uses the Y2's load addresses), `bootimg.check()`, ≤ 6 MiB | Same reviewed format as the Y2 images |

Assumptions the test exercises (INFERRED until it runs): LK places the kernel in the first 128 MiB of DRAM
(true on the Y2's LK); LK's frame buffer is `0x9fc00000` and RGB565 (stock `mtkfb` took that address; the
mainline MT6572 boards use RGB565 from LK), wrong colours or layout still show that the kernel runs; the
watchdog LK leaves does not expire before `mtk_wdt` probes. Failure modes: no change on screen (kernel did
not start or frame buffer elsewhere); a reboot loop (watchdog or panic, `panic=10`); a hang before `/init`
runs with the watchdog stopped, which with no power button may last until the battery is flat. In every case
the preloader still answers on the next start, so BOOTIMG can be restored with mtkclient.

Build result (2026-11-04, `out/y1-first-boot-2`, two fresh builds byte-identical): BOOTIMG 1,187,840 bytes,
sha256 `12861e2a2ad54afc92fb177c7fbfd259caea81c3dd0d47f7ce0d633b3e5dfb6d`; zImage+DTB `e91136a4…742fe`, DTB
`50b8b4c1…abac05`, `/init` `2fdf0cc7…5485`. `_text` at `0xc0008000`; `simplefb`, `mtk8250` and `mtk_wdt` built
in; no `CONFIG_Y2_*` set; the built-in archive holds `/dev/{console,tty0,watchdog}`, `/proc` and `/init`.

## First-boot result — 2026-11-04

The owner wrote the image to BOOTIMG (read back identical, `12861e2a…`) and booted it. **Linux 6.18 reached
userspace on the Y1.** Evidence: the owner's photo of the screen (`evidence-private/2026-11-04-y1-first-boot/`,
sha256 `e7cf5023…`), legible:

| Line on screen | Establishes |
| --- | --- |
| `simple-framebuffer 9fc00000.framebuffer: … format=r5g6b5, mode=480x360x16, linelength=960`; `Console: switching to colour frame buffer device 60x45`; legible white text | LK hands over a lit RGB565 480×360 frame buffer at `0x9fc00000` (CONFIRMED); unsigned BOOTIMG accepted; LK placed the kernel where `AUTO_ZRELADDR` works |
| `Memory: 502060K/509952K available` | The two RAM banks of the device tree work |
| `mtk-wdt 10007000.watchdog: Watchdog enabled (timeout=15 sec, nowayout=0)` | Mainline `mtk_wdt` drives the MT6572 watchdog |
| `Run /init as init process` at 0.077 s; `Y1 FIRST BOOT: Linux reached userspace…`, `watchdog: armed, fed every heartbeat`, `kernel: Linux version 6.18.56-y1-first-boot …`, `memory: MemTotal: 502772 kB`, `alive 5 s`, `alive 10 s` | PID1 runs, mounts proc, feeds the watchdog, sleeps and wakes on the MTK timer |
| `mt6577-uart 11005000.serial: probe with driver mt6577-uart failed with error -2` | Upstream `mt6572.dtsi` names only the `baud` clock; 6.18 `8250_mtk` then also requires `bus` (`mtk8250_probe_of`), so the UART driver does not bind. No effect on this test; fix in the board file next |

Not observed in the photo: the rest of the heartbeats and the reboot at 120 s.

## Second-boot result — 2026-11-05

Five runs of the second boot ([plan](../planning/Y1-SECOND-BOOT-PLAN.md) §9–13, photos in
`evidence-private/2026-11-04-y1-second-boot/`) established on the device:

| Observation | Establishes |
| --- | --- |
| The `mt6572-mainline` clock, pinctrl and PMIC-wrapper drivers, backported to 6.18.56, bind; the LK frame buffer stays lit with `clk_ignore_unused` | MT6572 SoC base for the Y1 (CONFIRMED) |
| `mtk-sd` reads `mmcblk0` (3696 MiB), `mmcblk0boot0`/`boot1` (2 MiB each) and the microSD once each has a fixed 3.3 V `vmmc` description | eMMC and SD host work at 50 MHz, 8/4 bit (CONFIRMED) |
| Six digests equal the backup at physical = dump − `0x600000` | The read path is correct and the backup is right (CONFIRMED) |
| `apt32f-wheel 0-0051: firmware 1`; wheel scroll `KEY_UP`/`KEY_DOWN`; reg3 bits 0–3 = bottom `KEY_PLAYPAUSE`, right `KEY_NEXTSONG`, top `KEY_BACK`, left `KEY_PREVIOUSSONG` | APT32F on I2C1 `0x51`, reset GPIO134, EINT2 on GPIO136 (needs `input-schmitt-enable` for the input direction), supply VGP1 3.0 V (CONFIRMED) |
| `key 28 ENTER (centre)` from `mtk-pmic-keys` | Centre button = MT6323 power key, PMIC interrupt EINT16 level-high (CONFIRMED) |
| No charging under the test kernels | Charging needs a charger/battery driver; not yet attempted |

## Third-boot result — 2026-11-05

Two runs of the third boot ([plan](../planning/Y1-THIRD-BOOT-PLAN.md) §10–11, evidence in
`evidence-private/2026-11-05-y1-third-boot/`):

| Observation | Establishes |
| --- | --- |
| Mainline MUSB glue + T-PHY, peripheral mode, PIO: `not attached` until the PHY forces the session (overlay `0015`, stock `usb_phy_recover` bits), then `configured`; the PC enumerates a high-speed `0525:a4a7` ACM gadget | USB device mode works on 6.18; the MT6572 PHY does not sense VBUS on this board (CONFIRMED) |
| MT6323 VUSB 3.3 V on; T-PHY slew-rate calibration skipped (`eye-src` 4) | Enough for high-speed enumeration (CONFIRMED) |
| AUXADC (`0x714` BATSNS, `0x716` ISENSE, `0x718` VCDT, `0x71a` BATON1, `0x71c` die): battery 3.69–3.80 V, ISENSE = battery (no current), VCDT pin ×369/39 = 113 mV unplugged / 5222 mV plugged, BATON1 553 mV, die 31–34 °C | Telemetry channels and the charger-input divider (CONFIRMED); BATON/NTC meaning still unvalidated |
| `vgp1: Bringing 2800000uV into 3000000-3000000uV` | LK leaves VGP1 at 2.8 V; stock kernel and Linux use 3.0 V |
| MT6323 chip ID `0x2023`, die-temperature fuses valid | Calibrated PMIC temperature available |
| Full kernel log over the USB shell (`run2-dmesg.txt`) | Logs no longer need photos |

## Offline disassembly: USB and charging — 2026-11-05

From the 3.0.2 kernel (`evidence-private/2026-11-04-y1-rom-3.0.2/rea`), read-only. CONFIRMED means seen in
the code, not on the device. Delays decoded from `__const_udelay` constants (HZ 100).

| Fact | Evidence | Status |
| --- | --- | --- |
| MUSB at `0x11100000`, IRQ 52 (SPI 20), `IRQF_TRIGGER_LOW`; DMA interrupt mask `0xff0000ff` at +0x200 | `musb_probe` c0792298, `musb_platform_init` c07937f8 | CONFIRMED (physical address INFERRED from the static map) |
| FIFO table: EP1–4 bulk 512 double, EP5–6 interrupt (TX 64/RX 512), EP7 bulk 512, EP8 ISO 512 double | table c07aa00c | CONFIRMED values, field meaning INFERRED |
| USB2 PHY at `0x11110800`; every PHY routine skips when `0x6b & 0x5c == 0x5c` (PHY in UART mode) | `usb_phy_poweron` c03a7820, `usb_phy_recover` c03a7ab4, `usb_phy_savecurrent` c03a7980 | CONFIRMED |
| Slew-rate calibration with the frequency meter at `0x11110f00`; on timeout writes 4 (`0x15 = 0x40`) | `hs_slew_rate_cal` c03a75ec | CONFIRMED |
| Efuse trims in recover: devinfo[18] bit 14 clears `RG_USB20_INTR_EN`; devinfo[7] bits 8–10 set `VRT_VREF_SEL` | `usb_phy_recover` | CONFIRMED |
| Clocks: `enable_clock(52, "PERI_USB")` = CG_CTRL1 bit 13; `enable_pll(2, "USB_PLL")` = UNIVPLL | `usb_enable_clock` c03a750c, `musb_platform_enable` | CONFIRMED |
| VUSB: `hwPowerOn(1, 3300, "VUSB_LDO")` at USB init; no normal-path power-down | `musb_platform_init` | CONFIRMED |
| `mt_usb_disconnect` power-cycles VGP1 (wheel supply): off, 5 ms, on at 3.0 V | `mt_usb_disconnect` | CONFIRMED |
| Charger present = PMIC `0x0000` bit 5, then `mt_usb_is_device` | `upmu_get_rgs_chrdet` | CONFIRMED |
| Charger type: MT6323 BC1.1 sequence on PMIC `0x24`/`0x26`; SDP, CDP, non-standard, DCP, Apple 2.1/1/0.5 A | `charging_get_charger_type` c03ece90 | CONFIRMED |
| Charging current: SDP/CDP 500 mA, non-standard/DCP 1000 mA, Apple 800/650/500 mA, unknown 70 mA | `select_charging_curret` | CONFIRMED |
| CV 4.325 V; pre-charge ends at 3.4 V; VBAT_OV_VTH 2, BATON and ULC detection on, charger watchdog on | `pchr_turn_on_charging` c037c420, `charging_hw_init` | CONFIRMED |
| AUXADC: BATSNS ch7 ×4, ISENSE ch6 ×4, charger input ch4 ×369/39, BATON1 ch5; 1.8 V, 15 bit | battery meter | CONFIRMED |
| Battery temperature: constant 25 °C (`force_get_tbat`); unused NTC math assumes 16.9 kΩ pull-up to 1.8 V, 27 kΩ pull-down | `force_get_tbat`, `BattVoltToTemp` | CONFIRMED |
| Fuel gauge SOC_BY_SW_FG; Q_MAX and R_I_SENSE in runtime custom data, not in the image | `battery_meter_initial` | UNRESOLVED |
| No external charger IC: no fan5405/bq24 code, no charger I2C device; MT6323 internal PCHR | symbols, `i2c_register_board_info` callers | CONFIRMED |

## Stock ROM 3.0.2

Source: `y1-community/y1-stock-rom` release 3.0.2, `rom.zip`
sha256 `7ee9aa5bf1c134f71e1f41d92cb90975f2225f9ad9db9617b392ddf653f764b1`. Vendor project
`g368_nyx` (`previous_build_config.mk`, `preloader_g368_nyx.bin`, `kernel_g368_nyx.bin`).
Extracted images, string dumps, the unpacked ramdisk and per-file hashes are kept in
`evidence-private/2026-11-04-y1-rom-3.0.2/` (not published). The package's
`MT6572_Android_scatter.txt` (`29390d04…`), `MTK_AllInOne_DA.bin`, `DA_PL.bin`, `DA_SWSEC.bin` and
`flash_tool.exe` match the `Y1Software/` rows of the [artifact manifest](artifact-manifest.tsv) byte for byte.
That match does not approve any of them for a Y1 operation.

| Area | Finding in the 3.0.2 ROM (CONFIRMED for the artifact) | Y2 counterpart |
| --- | --- | --- |
| Kernel | `kernel`: 512-byte MediaTek header, gzip zImage; `Linux version 3.4.5 … #1 SMP PREEMPT Tue Nov 11 16:53:08 CST 2025`, GCC 4.6 (google). No `IKCFG_ST` config. | Stock 3.4.67 ([baseline](stock-system-baseline.md)) |
| SoC / storage | Scatter `platform: MT6572`, `storage: EMMC`; `BOOTIMG` 6 MiB at `0x3180000`, `RECOVERY` 6 MiB, `ANDROID` `0x28a00000`, `USRDATA` 2 GiB | MT6582, eMMC ([partition map](partition-map.md)) |
| Panel | Two LCM drivers in kernel and LK: `gc9503v_hvga_dsi_vdo_hsd` and `st7701_hvga_dsi_vdo_boe`, both DSI video mode, ID-matched at boot (`lcm_compare_id`) | The same two names; Y2Linux drives both ([Beta 8.1](../validation/Y2-LINUX-1.0-BETA-8.1.md), `kernel/platform/panel.c`) |
| Wheel | `mtk-tpd: APT32F Touch Panel Device Probe`, `Focaltech APT32F I2C Touchscreen Driver`, neighbouring `UPDATE` client name | APT32F on I2C ([display/input](display-input.md), `kernel/platform/apt32f-wheel.c`) |
| Keys | MediaTek `kpd.c` (`GPIO_KPD_KCOL0..2_PIN` checks), accdet headset detection and hook key | Keypad / gpio-keys |
| Amplifier | `aw87559_pa`, `Speaker_Init Success aw87559` | AW87559 (`kernel/audio/aw87559.c`) |
| PMIC | `MT6323 PMIC CID=0x%x`, `CUST_EINT_MT6323_PMIC_*` | MT6323 |
| DAC | No external DAC name found (no CS43xx, ES9xxx, AK4xxx, TPA strings); stock audio is MediaTek `AudDrv` (`/dev/eac`) on the MT6323 analog codec | External CS43131 |
| Charger | PMIC charger-detect paths and one `FAN5405` string in the battery code | MT6323 charger ([charging policy](charging-current-policy.md)) |
| Radio | MediaTek BTIF/WMT strings (`mtk_wcn_btif_*`) | MT6627 CONSYS ([Wi-Fi/BT](wifi-bluetooth.md)) |
| USB / ADB | `default.prop`: `persist.sys.usb.config=mass_storage`, `ro.secure=1`, `ro.debuggable=0`; `adbd` present and started only by `sys.usb.config` triggers | ADB enabled on stock |

String presence shows that a driver is built in, not that the owner's board uses it: the panel
pair is ID-selected, the FAN5405 string can be dormant, and Type B Y1s have a different wheel.

## Offline disassembly of the 3.0.2 kernel — 2026-11-04

The gzip payload of `kernel` (Image sha256 `94eb0446…ae48b`) loads at `0xc0008000`; its kallsyms table
(42,751 text symbols at file offset `0x5f4f30`, 16-byte aligned sections) was recovered and wrapped into an
ELF for LLVM 22 `llvm-objdump`. Tools and outputs: `evidence-private/2026-11-04-y1-rom-3.0.2/tools` and
`rea/`. Call arguments come from immediates and literal-pool loads in the block before each call, checked
by hand; they are CONFIRMED for this image and INFERRED for the owner's player.

| Part | Y1 3.0.2 stock kernel | Y2 (this repo) |
| --- | --- | --- |
| Wheel controller | `i2c_register_board_info(1, {"APT32F", 0x51})` and `{"UPDATE", 0x60}` in `tpd_driver_init` 0xc0793024, after `hwPowerOn(5, 3000, "TP")`: index 5 is MT6323 **VGP1** in the 2013 MT6572 `mt_pm_ldo.h`, 3.0 V | `wheel@51` on **I2C0** (`kernel/dts/innioasis-y2.dts`) |
| Wheel reset | **GPIO134**: `tpd_probe` 0xc05944b8 drives 1, 10 ms, 0, 10 ms, 1; `tpd_suspend` holds it low; `tpd_resume` 1, 10 ms, 0, 20 ms, 1 | GPIO56, active low |
| Wheel data-ready | **GPIO136** mode 4, pull enabled, **EINT2**, `mt65xx_eint_registration(2, …, tpd_eint_interrupt_handler)` | EINT55 |
| Wheel protocol | `touch_event_handler` 0xc03aaf20 reads registers 1–6 one by one; a frame is valid when reg1 = 0x55. reg2 = 3: scroll, reg4 1–2 `KEY_LEFT`, 3–4 `KEY_RIGHT` (table 0xc05edd6c). reg2 = 1 press / 2 release: reg3 is a bitmask, bit0 `KEY_PLAYPAUSE`, bit1 `KEY_NEXTSONG`, bit2 `KEY_BACK`, bit3 `KEY_PREVIOUSSONG` (jump tables 0xc03ab144 / 0xc03ab284, values 1–9). Writes reg6 = 0x55 while a charger is present | Same frame (reg1 0x55, reg2 class, reg3 zone, reg4 scroll), but only class 3 is used; the five zones are GPIO lines (`kernel/platform/apt32f-wheel.c`) |
| Other keys | `kpd_keymap` 0xc0812664: `[0]` `KEY_VOLUMEDOWN`, `[9]` `KEY_VOLUMEUP`; PMIC power key `KEY_POWER` (232 in call state). No `KEY_ENTER`/`KEY_OK`/`KEY_SELECT` report anywhere in the kernel: the centre button's path is UNKNOWN. nugget-y1 maps a different code set (wheel `KEY_UP`/`KEY_DOWN`, centre `KEY_ENTER`), so stock codes changed between builds | Keypad with the same two volume keys |
| Amplifier | `aw87559_init` 0xc0793d04: `{"aw87559_pa", 0x58}` on **I2C0**; enable **GPIO143** high, 50 ms, then ID read (`aw87559_i2c_probe` 0xc0417b44); `aw87559_hw_init` writes the same 39-register table as `kernel/audio/aw87559.c` up to `0x01 = 0x30` | I2C1, enable GPIO8 |
| GC9503V panel | `lcm_init` 0xc03aa070: reset 1, 10 ms, 0, 10 ms, 1, 120 ms, then 40 rows at 0xc080cee4 whose 36 commands equal `gc9503v_init_cmds`, plus sleep-out, 120 ms, display-on, 20 ms | Same |
| ST7701S panel | `lcm_init` 0xc03aa354: reset 1, 20 ms, 0, 50 ms, 1, 150 ms, then 47 rows at 0xc080e564 whose 43 commands and delays equal `st7701s_init_cmds` | Same |
| Leftovers | `pn544` (I2C0 0x28), `dummy_cam_cal` (I2C1 0x55), `dummy_eeprom` (I2C1 0x50) from the reference design | Not fitted (INFERRED) |
| Memory | `mt_fixup` 0xc077ec10 builds the layout at boot from LK's memory tags (modem fixup, `DISP_GetVRamSizeBoot`); no fixed map in the kernel | Fixed D08 map ([RAM policy](initial-ram-map.md)) |

LCM tables are length-delimited (`push_table(table, n)`): `0xffe` is a delay row, and `0xfd` is a real
GC9503V command, not an end marker.

Boot loaders (same ROM): `lk.bin` is a 512-byte `LK` header plus 0x3d144 bytes loaded at **0x80020000**
(63 stored pointers land on string starts there, 4 at the neighbouring candidates), ARM vectors, mostly
Thumb-2 code. It contains the string `console=tty0 console=ttyMT1,921600n1 root=/dev/ram`, but the command
line it actually passes uses `ttyMT0` (see the ADB evidence boot above). Every DRAM entry of the preloader's
EMI table (from offset 0x16fac) is a single 512 MiB rank, so RAM is 0x80000000–0xa0000000 (INFERRED: the
entry chosen at boot is not identified). Reservations (modem, frame buffer, connectivity) are made at boot
by LK's tags and `mt_fixup`; LK's string references are built in code that linear disassembly does not
resolve, and a boot log (`[Phy Layout]` lines) would settle them directly.

## External sources

| Source | Identity | Use | Limits |
| --- | --- | --- | --- |
| [mt6572-mainline/linux](https://github.com/mt6572-mainline/linux) | `master` `8d427adfb611` (2026-06-13); branches `fg`, `b4/mt6323` | Mainline MT6572 SoC base: SMP, clocks, pinctrl, pwrap, MMC, MUSB, I2C, DSI DRM, Lima, MT6323 ADC/efuse/thermal | Phones only. Audio, charger, fuel gauge, cpuidle, SPM, connectivity, DBI/DPI not done; no MediaTek partition parser |
| [drsami/mt6572_kernel](https://github.com/drsami/mt6572_kernel) | tarball sha256 `fdc3ca4c…c7b7`, pushed 2013-12-20 | MediaTek 3.4.5 GPL drop: SPM/sleep, clocks, AFE, `AudDrv` analog codec, CONSYS reference | Board files belong to the `cci72_we_*` phones |
| [nugget-y1](https://github.com/norocoral/nugget-y1) | `c74420c6629b` (2026-11-02) | Community Y1 facts: 512 MB LPDDR2, 4 GB eMMC, MT6323 codec register notes, accdet, SPM deep idle | Rockbox fork; no kernel source (reuses the stock binary); says 3.4.67 where 3.0.2 ships 3.4.5; several Y1 tools carry no licence header |
| [y1-community/y1-stock-rom](https://github.com/y1-community/y1-stock-rom) | releases 2.1.9 to 3.1.2, `type-b-1.7.6` | Stock images for offline analysis | Community mirror; provenance from Innioasis not established |
| Innioasis Y1 kernel GPL source | not published | Board wiring | UNKNOWN; a GPL request is the owner's decision |

## Unknown before any Y1 implementation

- Pin modes and pulls of all other lines (set by the preloader's GPIO table, not by the kernel), the
  panel reset routing, accdet, the UART0 pads.
- What the 10 MiB at `0x9e000000` holds (modem or connectivity are the candidates); the
  [Y2 RAM policy](initial-ram-map.md) does not transfer.
- MT6323 configuration (regulator voltages, charger limits, battery table) of the Y1 board.
- Type B wheel controller identity.
- Recovery: a same-device user-area backup exists (above); no proven Y1 restore, no approved write procedure.
