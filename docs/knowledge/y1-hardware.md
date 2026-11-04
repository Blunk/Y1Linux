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
| Dump offsets equal table offsets: the user area starts with an `EMMC_BOOT` header and the preloader at `0x800` (unlike the Y2's 20 MiB shift) | CONFIRMED |
| Preloader, MBR, EBR1, LK, BOOTIMG and RECOVERY byte-identical to the public 3.0.2 ROM, so the kernel analysis applies to this player | CONFIRMED |
| Whole user area read once (sha256 `796cdf27…3497ef`); `0x0`–`0x3120000` (through SECCFG, including NVRAM and PROTECT_F/S) read twice, identical | CONFIRMED |
| eMMC BOOT1/BOOT2: both `--parttype` reads returned the user area's first 2 MiB; the legacy DA's partition switch is unchecked | NOT ACQUIRED |
| Any restore; an independent second copy | NOT PROVEN / owed |

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
Thumb-2 code. Its default command line is `console=tty0 console=ttyMT1,921600n1 root=/dev/ram`, so the
stock console is UART1 at 921,600 baud (the Y2 console is UART0). Every DRAM entry of the preloader's
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
  panel reset routing, accdet, the UART1 pads.
- The centre button's path, and whether the APT32F frame on the owner's player matches the 3.0.2 driver.
- Panel fitted to the owner's player (both tables equal the Y2's, so either is covered).
- The memory reservations LK and `mt_fixup` make at boot; the [Y2 RAM policy](initial-ram-map.md) does
  not transfer.
- MT6323 configuration (regulator voltages, charger limits, battery table) of the Y1 board.
- Type B wheel controller identity.
- Recovery: a same-device user-area backup exists (above); no proven Y1 restore, no approved write procedure.
