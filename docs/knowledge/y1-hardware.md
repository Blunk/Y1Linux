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

## External sources

| Source | Identity | Use | Limits |
| --- | --- | --- | --- |
| [mt6572-mainline/linux](https://github.com/mt6572-mainline/linux) | `master` `8d427adfb611` (2026-06-13); branches `fg`, `b4/mt6323` | Mainline MT6572 SoC base: SMP, clocks, pinctrl, pwrap, MMC, MUSB, I2C, DSI DRM, Lima, MT6323 ADC/efuse/thermal | Phones only. Audio, charger, fuel gauge, cpuidle, SPM, connectivity, DBI/DPI not done; no MediaTek partition parser |
| [drsami/mt6572_kernel](https://github.com/drsami/mt6572_kernel) | tarball sha256 `fdc3ca4c…c7b7`, pushed 2013-12-20 | MediaTek 3.4.5 GPL drop: SPM/sleep, clocks, AFE, `AudDrv` analog codec, CONSYS reference | Board files belong to the `cci72_we_*` phones |
| [nugget-y1](https://github.com/norocoral/nugget-y1) | `c74420c6629b` (2026-11-02) | Community Y1 facts: 512 MB LPDDR2, 4 GB eMMC, MT6323 codec register notes, accdet, SPM deep idle | Rockbox fork; no kernel source (reuses the stock binary); says 3.4.67 where 3.0.2 ships 3.4.5; several Y1 tools carry no licence header |
| [y1-community/y1-stock-rom](https://github.com/y1-community/y1-stock-rom) | releases 2.1.9 to 3.1.2, `type-b-1.7.6` | Stock images for offline analysis | Community mirror; provenance from Innioasis not established |
| Innioasis Y1 kernel GPL source | not published | Board wiring | UNKNOWN; a GPL request is the owner's decision |

## Unknown before any Y1 implementation

- Y1 GPIO, EINT and I2C assignments (wheel bus/address, reset and data-ready lines; amplifier enable; panel reset).
  They are compiled into the 3.4.5 binary; no device tree exists.
- Panel fitted to the owner's player, and whether the Y1 command tables equal the Y2 tables.
- LK memory map and command line handed to the kernel; the [Y2 RAM policy](initial-ram-map.md) does not transfer.
- MT6323 configuration (regulator voltages, charger limits, battery table) of the Y1 board.
- Type B wheel controller identity.
- Recovery: no same-device backup, no proven Y1 restore, no approved Y1 download agent.
