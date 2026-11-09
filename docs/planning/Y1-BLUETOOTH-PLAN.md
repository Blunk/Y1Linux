# Y1 Bluetooth

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The step builds named
> below (the *N*th boot, its variant, tree, fragment and PID1 header) are the record of the test images that ran
> on the player; their files are folded into that build.

Status: **B0–B4 done, B5 PASS** (2026-11-06, owner-approved): firmware, WMT boot and RF calibration on the Y1 without the modem; B6 (hci0) not admitted; B7 not needed.
(its connectivity driver with firmware from the stock system image). New hardware scope (a radio, a new power
domain, a memory-layout change): each stage has its own admission, and the roadmap audit is repeated before stage
B3 (memory) and B4 (first power-on).

## 1. Evidence

Read-only research (agent notes `out/y1-bluetooth-notes.md`, file:line and addresses there; stock files extracted
read-only into a scratch directory, never into the repo). CONFIRMED unless marked.

- **The Y2** (`kernel/platform/connectivity/`, about 1.5k lines plus a Wi-Fi subtree; DT
  `innioasis-y2.dts:297-319`; the M5 connectivity documents): power-on boots the MD1 modem for RF calibration
  first (`md.c`, `modem_1_2g_n.img`), switches the SPM CONN domain (`0x10006280`, key `0x0b160001`), VCN18 on and
  VCN28 off (co-clock), remaps the radio core's memory, polls the chip ID at MCU+8 (`0x6582`); WMT boot with a
  two-part "ALPS" patch and RF calibration (`wmt.c`); a standard `hci0` (`hci.c`); BlueZ above it. Five firmware
  files from the owner's stock `system.img` (`/etc/firmware`), pinned by size and SHA-256, installed to
  `/lib/firmware/mediatek/mt6582/` by `tools/connectivity/provision.py`; no blobs in the repo (redistribution not
  established).
- **The MT6572 has the same connectivity core**: the stock Y1 `mtk_stp_wmt.ko` is built from the same `conn_soc`
  tree, reads the chip ID at `0x18070008` and accepts `0x6572` and `0x6582`; the stock `6620_launcher` names
  `mt6572`, `mt6582` and the `mt6572_82` patch prefix; the stock power-domain table has CONN at `0x10006280`
  (status `0x2`, bus protection `0x11`) and MD1 at `0x284`, the same sequence as the Y2's; the same MT6323 rails.
- **What differs**:

  | Item | Y2 (MT6582) | Y1 (MT6572) |
  | --- | --- | --- |
  | BTIF | `0x1100c000`, SPI 50 | `0x1100e000`, SPI 52 |
  | BTIF DMA TX / RX | `0x11000780` / `0x11000800` | `0x11000380` / `0x11000400`, SPI 41 / 42 |
  | BTIF wake | SPI 185 | SPI 122 |
  | Radio core memory | `0xbdf00000`, remapped | stock `0x80000000`, 1 MiB (the Y1 kernel uses it today) |
  | Patch | two parts, "ALPS" | one part (`0x11`, address `0x0600`), "1319"; HW/FW `8a00` as the Y2 |
  | `WMT_SOC.cfg` | `co_clock_flag=1` | no `co_clock_flag`, adds `sdio_driving_cfg=0` |
  | Wi-Fi | firmware present | **none** (no `WIFI_RAM_CODE`, no `wlan.ko`): Bluetooth only |
  | `BT_Addr` record | 64 bytes | 32 bytes, non-zero address |

- **The Y1's stock firmware** (the owner's backup system partition, `/system/etc/firmware`):

  | File | Bytes | SHA-256 |
  | --- | --- | --- |
  | `WMT_SOC.cfg` | 82 | `161f18b686e59a763368ffb1d8229f158fa61493ecc3dff5c6ef8c11a7f5f1e0` |
  | `mt6572_82_patch_e1_0_hdr.bin` | 39868 | `5c13ebad70fbddc10410c675fdce84e89f5db9fa1323f9d787eca3e96a99bfc1` |
  | `modem_1_2g_n.img` (MT6572 modem, MOLY.WR8.W1315) | 2391624 | `31940b2f14ad30b287f3ce22c5f6e345523d55c9150fadde2c7ca3e48eab4b79` |

  Stock init loads the WMT/STP/BT modules and runs `6620_launcher -p /system/etc/firmware/`; the MD1 modem runs
  permanently on stock (`init.modem.rc`). The stock Bluetooth stack is MediaTek's BlueAngel (not needed with BlueZ).
  The modem calibration records in PROTECT_F/S have the Y2's layout.
- **No mainline or fork driver**: mainline 6.18 has none for this core (`btmtkuart`/`btmtksdio` use other
  transports); the MT6572 fork comments out the CONN power domain and only reserves `connsys@80000000`.
- **Unknown**: whether the MT6572 needs a full MD1 modem boot before Bluetooth RF calibration, as the Y2 did
  (`docs/knowledge/reverse-engineering-audit.md:109`). The stock power-on also touches pad and memory-controller
  registers (`0x10005300`, `0x10208000`, `0x10000084` bit 26) not yet decoded (INFERRED).

## 2. Design

Adapt the Y2's `kernel/platform/connectivity/` rather than write a new driver: chip ID `0x6572`; a one-part patch
loader for the Y1's header; the Y1's `WMT_SOC.cfg` key set and VCN28/26 MHz handling without co-clock; the Y1's
BTIF and DMA addresses and IRQs; the CONN power domain in `kernel/y1/mt6572-pm-domains.h` (as MFG for the GPU), a
CONN reset and the memory remap; no Wi-Fi subtree, no VCN33-WIFI. Firmware: a Y1 profile in `provision.py`
(`/lib/firmware/mediatek/mt6572/`, the three files pinned above), extracted from the owner's own device, never
committed. Userspace (BlueZ, BlueALSA, Reborn's Bluetooth screens) as on the Y2; SBC first.

## 3. Stages

| Stage | What | Writes to the Y1 |
| --- | --- | --- |
| **B0** | This plan, the roadmap audit, the provisioning profile and inventory | none |
| **B1** | Offline decode of the stock MT6572 power-on (pads, memory controller, remap, CONN reset, VCN28), the WMT command sequence, whether the stock stack triggers the modem or NVRAM reads, the Bluetooth address injection | none |
| **B2** | The Y2 driver adapted (built, not loaded): addresses, chip ID, patch loader, config parser, power-domain glue; host tests as the Y2's | none |
| **B3** | Reserve the radio core's 1 MiB (moving the kernel off `0x80000000`, or remapping the core elsewhere) and the BTIF buffers; a boot with nothing else changed | memory layout only |
| **B4** | Power on to the chip ID only (`0x18070008` = `0x6572`), no firmware, then power off | the CONN domain, rails, clocks |
| **B5** | WMT boot, the single patch, RF calibration; whether it succeeds without the modem | firmware into the radio core |
| **B6** | `hci0`, scan; the address from the 32-byte record (read-only) or a generated local one; then Bluetooth headphones through BlueZ/BlueALSA and Reborn | radio on |
| **B7** | Only if B5/B6 fail without it: the MD1 modem calibration, ported for the MT6572 | modem |

## 4. Risks

The modem calibration (B7) is the largest unknown and roughly doubles the work if needed. Writes to untested power,
pad and memory-controller registers can hang the bus (the watchdog resets; the image is restored with mtkclient).
A wrong VCN28/26 MHz setting can disturb the system clock. The memory reservation collides with today's kernel
placement. Per-unit calibration data in PROTECT_F/S and the address record are read-only, never written. The
firmware's licence: owner-local extraction only, as on the Y2. CPU and RAM cost of Bluetooth audio on the
MT6572 (512 MiB). Effort (INFERRED): about 25–35 % of the Y2's kernel bring-up without the modem step (the Y2's
driver took about six weeks), 60–80 % with it; the userspace and Reborn work carries over.

## 5. Admission

| Stage | Item | Bound |
| --- | --- | --- |
| B0–B2 | **Prepare** | documents, host code and tests; nothing on the Y1 |
| B3 | **Memory layout** | the reservation only; a regression boot |
| B4 | **Power on to the chip ID** | the CONN domain, its reset, VCN18/VCN28 as decoded in B1, clocks; read the ID, power off |
| B5 | **Firmware** | the owner's own stock patch and config, pinned by SHA-256; WMT boot and calibration |
| B6 | **Bluetooth on** | `hci0` and audio; the factory address read-only |
| B7 | **Modem calibration** | only with its own plan, if needed |

## 6. B0 — 2026-11-06

Owner-approved (B0–B2). `docs/knowledge/evidence/y1-firmware-inventory.json`: the three files with sizes, SHA-256,
the patch header (`20130523111343a`, platform "1319", HW/FW `8a00`, info `0x11000600`: one part, sequence 1,
address `0x0600`), install paths under `/lib/firmware/mediatek/mt6572/`, redistribution not established.
`tools/connectivity/provision.py --profile y1`: the Y1 inventory, the one-part "1319" patch rule (the Y2 profile's
two-part "ALPS" rule unchanged, its check widened from `PS` at bytes 18–19 to `ALPS` at 16–19), no Y2 default
records for the Y1, the profile in the receipt. `tests/test_y1_provision.py` (4 cases, synthetic files): OK. A
dry run with the owner's real stock files (scratch directory, not the repo): all three verified and staged. The
existing `tests/test_connectivity.py` errors only where the host lacks `clang` and a download cache, as before.

## 7. B1 — 2026-11-06

Offline decode of the stock `mtk_stp_wmt.ko`, `mtk_stp_bt.ko`, `6620_launcher`, `libbluetooth_mtk.so` and kernel
(notes `out/y1-bluetooth-b1-notes.md`, every item with its stock address). CONFIRMED unless marked:

- **No remap, no separate reset**: the radio core's memory is fixed at `0x80000000` (1 MiB); the only reset is
  `RST_B`, bit 0 of the SPM CONN register `0x10006280`. The `0x10208000` (IOCFG pads) and `0x10005300` (GPIO
  mode) writes are JTAG debug code behind a flag, **never to be written**. `0x10000084` bit 26 un-gates the SPM
  clock (TOPCKGEN CG_CTRL1 clear; BTIF bit 12, APDMA bit 2 in the same group).
- **Power-on**: PMIC `0x512` bit 1 = 0, VCN_1V8 on; no co-clock (the Y1's config), so `0x41c` bit 14 = 1 (VCN28
  hardware control) and VCN28 on; the SPM clock, key `0x0b160001`; the CONN domain `|=4` (1 µs), `|=8` (3 µs), poll
  bit 1 of `0x60c`/`0x610`, `&=~0x10`, `&=~2`, `|=0x10`, `|=1`, `&=~0x10`; bus protection mask `0x11` released on
  `0x10001220`/`0x228`; 10 µs; poll `0x18070008` for `0x6572` (9 × 20 ms), 5 ms. Power-off waits for SPM
  `0x100063b4` bit 11 (10 s), the reverse sequence, VCN18/VCN28 off. Against the Y2: no remap, no reset line, no
  CONNMCU clock, mask `0x11` (Y2 `0x104`), VCN28 on (Y2 off), the extra isolation toggle and delays.
- **BTIF/DMA**: the Y2's register model and values; addresses BTIF `0x1100e000` SPI 52, TX DMA `0x11000380` SPI 41,
  RX DMA `0x11000400` SPI 42, wake SPI 122 level-low.
- **WMT**: the command tables are byte-identical to the Y2's `wmt.c`; one patch (byte 24 `0x11`, "1319"), address
  `00 00 06 00`; HVR `0x8a00`/`0x8a01` both use the `_e1` patch; stock also sends an FM strap command (`01 05 02 00
  02 <fm>`, value INFERRED 2; not needed for Bluetooth at first); RF calibration with **both** 3.3 V PA rails
  (VCN33_BT and VCN33_WIFI) on for its duration (the plan's "no VCN33-WIFI" corrected: that rail only during
  calibration); stock does not check the calibration result.
- **Modem**: nothing on the Android side needs it (no CCCI/MD imports, no NVRAM or modem strings in the
  launcher); whether the radio firmware does is decided in B5 (INFERRED: the Y2's need came with co-clock).
- **Address and settings**: `0xfc1a` (address), `0xfc79` (record bytes 12–17), `0xfc93` (27–29), `0xfc7a` (18–24),
  then HCI Reset; 30 of the 32 record bytes used; offsets and order as the Y2's `hci.c`.
- **Memory**: the kernel decompresses to `0x80008000` today, inside the radio's MiB (`AUTO_ZRELADDR` forced,
  `PHYS_OFFSET` `0x80000000`). Recommended for B3: `TEXT_OFFSET 0x00108000` (a small Kconfig/Makefile overlay, as
  mainline does for `ARCH_REALTEK`) and a `no-map` reserved-memory node `0x80000000`/1 MiB: the kernel at
  `0x80108000`, where stock's runs. Rejected: stock's `PHYS_OFFSET 0x80100000` (2 MiB phys/virt alignment) and moving
  the radio memory (no remap register).
- **B4's read-only precondition**: SPM `0x10006280`, `0x284`, `0x60c`, `0x610`, `0x3b4`; INFRA `0x10001220`, `0x228`,
  `0x310`, `0x300`–`0x30c`; TOPCKGEN `0x10000024` bits 2, 12, 26; EMI MPU `0x10004160`–`0x1bc`; PMIC `0x102`, `0x120`,
  `0x148`, `0x402`, `0x512`, `0x41c`, `0x41a`, `0x416`, `0x418`, `0x420`. Never read `0x18070000` or BTIF/DMA while
  unpowered or unclocked.

## 8. B2 — 2026-11-06

The Y2 driver adapted for the MT6572, compiled into a Y1 kernel with **no device-tree node** (nothing can probe,
nothing powers on); no memory reservation, no `TEXT_OFFSET` change (both B3); nothing built for or sent to the Y1.

- **Shared unchanged** (pinned in `kernel/patches-y1/manifest.json` as "added" files under `drivers/bluetooth/`, as
  the wheel driver is): the Y2's `btif.c`, `stp.c`, `hci.c`, `firmware.c`, `protocol.h`. They compile against the
  Y1's `kernel/y1/connectivity/conn.h` (same names and include guard, Bluetooth fields only, `bt_factory[32]`). The
  only Y2 edits move per-SoC data into the Y2's `conn.h`: the firmware table (`Y2_CONN_FIRMWARE`) and the generic
  address hash (`Y2_CONN_DEFAULT_ADDRESS_SHA256`). **Evidence the Y2 is unchanged**: production kernel built
  before and after (`out/b2-y2-before`, `out/b2-y2-after`): `Image` `a68fdffd…aac21`, `vmlinux` `b5ae438d…eb202`
  and all seven connectivity objects byte-identical. Y2 config and DT untouched.
- **Y1 copies** (`kernel/y1/connectivity/`): `core.c` (almost every power step differs: no modem, no RGU reset, no
  EMI remap, no CONNMCU clock, VCN28 on in hardware mode, the SPM clock never gated again, the domain sequence,
  BTIF opened after `RST_B`, the sleep handshake before power-off; Bluetooth only; the 32-byte record hand-off;
  DT windows, interrupts and the memory region checked before any mapping) and `wmt.c` (chip `0x6572`, HVR
  `0x8a00`/`0x8a01`, one patch, the Y1 key set, no co-clock command, no FM strap). `wmt.c` repeats the Y2's command
  layer (`y2_conn_wmt`, `read_version`, `y2_wmt_function`) verbatim: in the Y2 it shares a file with the MT6582
  boot, and splitting that file is a Y2 structure change (its own commit, CODING-STANDARD §7), proposed, not done.
- **`y1-conn-policy.h`**: the Y1's facts as pure data for the driver and the host tests: windows and SPI numbers
  (BTIF 52, TX/RX DMA 41/42, wake 122), the CONN on/off sequences, the config key set, the single-part header
  rule, the record rule and the MT6572 default-address hash (`00:00:46:65:72:01` in stock `libbluetooth_mtk`,
  the Y2's is `…:65:82:01`; stored only as a hash).
- **CONN domain in the driver, not in `mt6572-pm-domains.h`**: mainline's generic `scpsys_power_on` cannot express
  stock's clock-isolation pulse around `RST_B` (bit 4 set again while bit 0 is released, then cleared) nor its
  1/3 µs settle times, and the MT6572 binding has no CONN index (commented out in the fork). The driver writes
  the SPM through the existing scpsys syscon (`innioasis,scpsys`) and the bus protection through `mediatek,infracfg`
  (regmap-locked; MFG uses other registers). A missing acknowledgement stops the sequence and isolates (stock
  WARNs and continues). Bit names follow mainline (bit 1 ISO, bit 4 CLK_DIS); the B1 notes name them the other
  way round, values identical. Never `0x10208000`/`0x10005300` (a test greps the Y1 sources).
- **Build**: overlays 0036/0037 (`drivers/bluetooth/Kconfig`, `Makefile`: `CONFIG_BT_INNIOASIS_Y1`, bool). No
  new variant: the thirtieth fragment plus `CONFIG_BT=y`, `CONFIG_BT_BREDR=y`, `CONFIG_BT_LE=y`,
  `CONFIG_BT_INNIOASIS_Y1=y`, the thirtieth DT unchanged (`out/b2-y1-final`): zImage
  `026fcadaa1af24145be768199583d5b844ee2bc478aec549fd5d44bf3d35aca9`, `.config` `79d8fa69…6e26`; no warning, the six
  driver objects again with `-Werror` and with `W=1`: none. The thirtieth fragment alone still yields its recorded
  `.config` (`577f2792…e17e`): the overlays change nothing while Bluetooth is off.
- **Tests**: `tests/test_y1_connectivity.py` (13, in the build sandbox for clang): the wiring table; the domain
  sequences against the B1 transcription and a register model (`RST_B` released with the clock isolated, bus
  protection held until the domain is up, 1/3 µs and 1/1 µs); the actual `board_config` with the owner's exact
  82-byte config (its inventory SHA-256) and nine variants; the header rule on the stock header bytes (checked
  against the owner's file offline) and the actual `patch()` download (P_ADDRESS `00 00 06 00` only in the second
  command, 1000-byte fragments 1/2/3, RESET; it found and now guards a first-draft bug that put the address into
  the first command); the record rule; the firmware table against the inventory; the shared `hci.c` reading only
  record bytes 12–29; the shared files pinned; no Y1 tree names the driver. Unchanged results: `test_connectivity`
  (the regulatory-download error only, as before), `test_overlays`, `test_y1_provision`, `test_cpu_final`,
  `test_wifi`; `test_overlay_hashes` OK on the host (in the sandbox the Y2 overlays on `/src` fail the other set,
  as before). `code-quality.py`: no regression.
- **For B3/B4**: the Y1 DT node (compatible `innioasis,y1-mt6572-connectivity`; reg names `mcu`, `conn-emi`, `btif`,
  `tx-dma`, `rx-dma`; interrupt names `btif`, `tx-dma`, `rx-dma`, `wake`, `GIC_SPI n IRQ_TYPE_LEVEL_LOW`; clocks
  `spm`, `btif`, `dma` = `CLK_TOP_SPM`, `CLK_TOP_BTIF`, `CLK_TOP_APDMA`; supplies `vcn18`, `vcn28`, `vcn33-bt`,
  `vcn33-wifi`, which the Y1 tree does not yet describe as regulators; `innioasis,pwrap`, `innioasis,scpsys`,
  `mediatek,infracfg`; `memory-region` the B3 `no-map` node at `0x80000000`/1 MiB, which `probe` requires). Open
  and INFERRED until B4/B5: whether the sleep bit clears without a WMT sleep command; the BTIF/DMA interrupt
  polarity; the 5 s PMIC settle carried from the Y2; the calibration event length (a longer event fails).


## 9. B3 build — 2026-11-06

Owner-approved; roadmap audit repeated first (`roadmap-gap-audit.md`, "Y1 Bluetooth B3, memory layout").
Overlays `0038-mt6572-reserve-conn-kconfig.patch` (`MACH_MT6572_RESERVE_CONN`, under `MACH_MT6572`) and
`0039-mt6572-reserve-conn-textofs.patch` (`textofs-$(CONFIG_MACH_MT6572_RESERVE_CONN) := 0x00108000`, beside
mainline's Realtek line). Device tree `innioasis-y1-thirty-first-boot.dts`: `reserved-memory` with
`connsys@80000000` (1 MiB, `no-map`). Config: the thirtieth fragment plus `MACH_MT6572_RESERVE_CONN`
(`AUTO_ZRELADDR` as before, `ARM_PATCH_PHYS_VIRT` off, so `PHYS_OFFSET` stays `0x80000000`): `_text` at `0xc0108000`
(physical `0x80108000`, stock's). No Bluetooth driver, no radio power. Build `out/y1-thirty-first-boot-1`, BOOTIMG
sha256 `dcaa1fcdbc7b815929a6f93f6216c7f22ebf2480d86bf3d3a7b63358bcd9e6ed`.

**B3 run** (`evidence-private/2026-11-06-y1-bt-b3/run1.txt`): the thirty-first boot came up normally (the splash,
Reborn, MTP, Y2DATA, charging CC at 450 mA, no new warning). `OF: reserved mem: 0x80000000..0x800fffff (1024 KiB)
nomap non-reusable connsys@80000000`; memory nodes `0x80000000–0x801fffff` and `0x80200000–0x9dffffff`; MemTotal
497 320 kB (`free` 486 MiB, was 487): 1 MiB to the radio core as planned. `/proc/iomem` lists System RAM from
`0x80200000` and no "Kernel code" line: the first 2 MiB section holding the reservation and the kernel's text is not
listed as a resource (cosmetic; the kernel runs from it). Owner's regression check pending.

## 10. B4 build — 2026-11-06

Owner-approved after B3 passed; roadmap audit for B4 written first. Read-only baseline
(`evidence-private/2026-11-06-y1-bt-b4/baseline.txt`): CONN `0x10006280` = `0x1112` (off), PWR_STATUS `0x3ffd`
(CONN bit 1 clear; MD1 on from LK, `0x284` = `0x0d`), `0x100063b4` = `0x080000f9` (bit 11 clear), INFRA `0x1220` = 0,
`0x1228` = `0xf000`, `0x1300`–`0x1310` = 0 (no remap), TOPCKGEN `0x10000024` = `0xd8cfdaff` (SPM bit 26 ungated,
BTIF bit 12 and APDMA bit 2 gated), EMI MPU 0, PMIC `0x102` = `0003`, `0x120` = `0001`, `0x148` = `0004`, `0x402` =
`8400`, `0x512` = 0, `0x41c` = 0, `0x41a` = `0044`, `0x416` = `001c`, `0x418` = `0420` = 0.

- **Driver** (`kernel/y1/connectivity/core.c`, `y1-conn-policy.h`): the power-on is a list of named stages
  (`y1_conn_full_stages`, `y1_conn_chip_id_stages`). DT `innioasis,bring-up = "chip-id"` (absent: the full driver;
  any other value: probe refuses) runs, at probe and asynchronously: the resource check (windows, interrupts,
  `no-map` memory) → normalise (CONN off if found on) → VCN18 (`0x512` bit 1 = 0, on) → VCN28 (`0x41c` bit 14 = 1,
  on, 150–250 µs) → clear the 1 MiB radio memory → SPM clock → key and the CONN domain sequence with the bus
  protection released → 10 µs, poll `0x18070008` for `0x6572` (20 ms steps, 200 ms) → power-off: the sleep wait
  (`0x100063b4` bit 11, 100 ms steps, 10 s, then isolation anyway), the reverse domain sequence (every step even
  after a missing acknowledgement, as stock), VCN18 off, `0x41c` bit 14 = 0, VCN28 off. It then returns `-ENODEV`
  (silent; the device stays unbound, holding nothing). It takes only the SPM clock and VCN18/VCN28: no PA rail,
  no BTIF/DMA mapping, clock or register, no interrupt, no firmware, no HCI, no sysfs. Not done in this mode:
  the PMIC init subset (`pmic_prepare`; the baseline shows LK has not applied it: a B5 decision, as it touches
  the 26 MHz/TCXO policy). HVR/FVR are WMT register reads (BTIF + firmware): B5. Never `0x10208000`/`0x10005300`.
  The SPM clock stays enabled (stock never gates it again).
- **Tests** (`tests/test_y1_connectivity.py`, 15, sandbox): the stage lists; and the driver's actual power path
  (14 functions extracted from `core.c`) against a register model seeded with the baseline above: stock order
  of rails, memory, clock, domain, ID, protection, rails off; the MCU window read only with the domain fully up;
  PMIC writes only to `0x512` bit 1 and `0x41c` bit 14; no BTIF, no WMT, no PA rail; back to the baseline values
  after a found ID, a missing ID (`-ETIMEDOUT`, still off), a core that never sleeps (one warning) and a CONN
  found on.
- **Variant thirty-second**: `kernel/config/y1-thirty-second-boot.config` (the thirty-first fragment plus
  `CONFIG_BT`, `BT_BREDR`, `BT_LE`, `BT_INNIOASIS_Y1`; LOCALVERSION `-y1-thirty-second-boot`),
  `kernel/dts/innioasis-y1-thirty-second-boot.dts` (the thirty-first tree plus `connectivity@18070000` with the
  chip-id limit, `memory-region = <&connsys_memory>`, and `ldo_vcn18` 1.8 V / `ldo_vcn28` 2.8 V without
  always-on or boot-on: both fixed LDOs, off as LK leaves them, so `regulator_late_cleanup` has nothing to switch
  off and no voltage is written; no VCN33 node), `tools/build/y1-boot.sh`, `tools/build/y1_boot.py` (one more
  enabled device, the node's lines, regulators exactly vgp1, vusb, the card rails, vcn18, vcn28; `vcn33` and
  `regulator-boot-on` forbidden). Build `out/y1-thirty-second-boot-1`: BOOTIMG
  `1c52a754b9cd8184203b17d16990b015c88215d5bbecd30df1917fdfa8799cbb`, `0x383800` bytes (3 684 352), zImage-dtb
  `78b8021a…d596`, DTB `497d3209…3d95`, `.config` `30c30ea4…b76a`, `y1-init` unchanged (`699cd02e…`); no compiler
  warning, only the fork's two known DT warnings.

**Expected dmesg** (`dev_info`, below the console's loglevel 3: `dmesg | grep -i 18070000`):

    y1-connectivity 18070000.connectivity: bring-up chip-id: CONN on, chip ID, CONN off
    y1-connectivity 18070000.connectivity: CONN chip=6572 (register 0x....6572)
    [possible] y1-connectivity 18070000.connectivity: CONN did not sleep within 10 s; isolating
    y1-connectivity 18070000.connectivity: bring-up chip-id: off: ctl=0x1112 status=0x3ffd sleep=0x... protect=0x11
    y1-connectivity 18070000.connectivity: bring-up chip-id: done, chip=6572; HVR/FVR need WMT (B5)

No "probe … failed" line on success. Without the ID: `CONN chip ID absent: 0x… after 200 ms`, `connectivity
stage 6 failed: -110 (power-off 0)`, the `off:` line and `probe with driver y1-connectivity failed with error
-110`. Any `CONN on step` / `CONN off step … failed` line names the register whose acknowledgement was missing.
The sleep warning is possible (whether the ROM sleeps unprompted is unknown) and harmless.

**Post-boot read-only checks** (as the baseline; never read `0x18070000` or BTIF/DMA from userspace):

| Register | Expected | Note |
| --- | --- | --- |
| SPM `0x10006280` | `0x1112` | CONN back to the baseline |
| SPM `0x1000660c` / `0x10006610` | `0x3ffd`; bit 1 clear in both | MD1 bit 0 still set |
| SPM `0x10006284` | `0x0d` | MD1 untouched |
| SPM `0x100063b4` | bit 11 clear | |
| INFRA `0x10001220` / `0x10001228` | `0x11` / `0xf011` | **differs from baseline (0 / `0xf000`)**: stock's power-off leaves the CONN bus protection set |
| TOPCKGEN `0x10000024` | `0xd8cfdaff` | SPM ungated; BTIF and APDMA still gated (never touched) |
| PMIC `0x512`, `0x41c` | `0000`, `0000` | VCN18, VCN28 off, hardware mode cleared |
| PMIC `0x416`, `0x418`, `0x420`, `0x41a` | `001c`, `0000`, `0000`, `0044` | PA rails and VOSEL untouched |
| PMIC `0x102`, `0x120`, `0x148`, `0x402` | `0003`, `0001`, `0004`, `8400` | init subset not applied |
| `/sys/bus/platform/drivers/y1-connectivity/` | no `18070000.connectivity` link | unbound after the bring-up |
| `/sys/class/bluetooth/` | empty | no hci0 |
| `/sys/class/regulator/*/name` vcn18, vcn28 | `state` disabled | |

Then the thirty-first's regression check (splash, Reborn, MTP, Y2DATA, charging) unchanged.


## 11. B4 run — PASS — 2026-11-06

`evidence-private/2026-11-06-y1-bt-b4/run1.txt`, thirty-second boot: `bring-up chip-id: CONN on, chip ID, CONN off`
at 1.56 s; **`CONN chip=6572 (register 0x00006572)`** at 1.66 s; `CONN did not sleep within 10 s; isolating` (expected
without firmware: nothing sends the core to sleep; the probe is asynchronous, the boot is not delayed); `off:
ctl=0x1112 status=0x3ffd sleep=0x80000f9 protect=0x11`; `done, chip=6572; HVR/FVR need WMT (B5)`. Afterwards (read
only): SPM CONN `0x1112`, MD1 `0x0d`, `PWR_STATUS` `0x3ffd` (both copies), `0x100063b4` `0x080000f9`, INFRA
`0x10001220` `0x11` and `0x10001228` `0xf011` (the CONN bus protection left set, as stock's power-off), TOPCKGEN
`0xd8cfdaff`, every PMIC register at its baseline, `vcn18` and `vcn28` disabled, the driver unbound, no Bluetooth
device; Reborn and MTP running. Open for B5: HVR/FVR (WMT), whether the core sleeps after a WMT sleep command, the
calibration (with VCN33_BT/WIFI for its duration), the modem question.

## 12. B5 build — 2026-11-06

Owner-approved after the B4 run. The WMT boot with the owner's firmware, started from userspace, one attempt per
write, observed step by step; no HCI device, no address, no modem.

- **Driver** (`kernel/y1/connectivity/`): DT `innioasis,bring-up = "wmt"`: probe claims everything (the SPM, BTIF and
  APDMA clocks, VCN18/VCN28/VCN33_BT/VCN33_WIFI, the windows, the BTIF/DMA buffers and interrupts, not enabled) and
  powers nothing; it adds `bring_up`, `power_off` (both write-only) and `status` (read-only). One attempt (`bring_up`,
  synchronous, the lifecycle lock held): the PMIC init subset once per boot (stock's and the Y2's, values logged
  before/after; then the Y2's 5 s settle) → the B4 stages → `btif` (the BTIF and APDMA clocks on, then the
  registers, then the interrupts: the Y2's `y2_btif_start`) → `wmt`: `WMT_SOC.cfg` (the Y1 key set), HVR/FVR/chip,
  STP to full mode, the single patch (P_ADDRESS `00 00 06 00` in the second command only), RF calibration with
  VCN33_BT and VCN33_WIFI on for its duration only, coexistence; no co-clock command, no FM strap → WMT sleep
  (`01 03 01 00 01`, reply `02 03 02 00 00 01`, INFERRED from stock STP-PSM; a refusal is logged and the power-off
  goes on) → the B4 power-off (sleep wait, domain off, rails off; BTIF stopped and its clocks gated first). Each
  stage and each WMT command but the patch fragments is logged at info level. A failure powers off at once and
  leaves the device bound for the next attempt. `power_off` aborts: the running attempt's WMT wait ends, no further
  stage or command runs, the power-off follows. The calibration event may have **any length** when well formed
  with status 0 and operation 1 (`y1_conn_rf_result`; only the first six bytes are kept, length and status logged).
- **Shared Y2 sources**: `protocol.h` gains a default `Y2_CONN_RF_RESULT` (the MT6582 rule) that the Y1's `conn.h`
  replaces; `stp.c` calls it. Y2 production build after the change (`out/b5-y2-after`): `Image` `a68fdffd…aac21`,
  `vmlinux` `b5ae438d…eb202` and all seven objects identical to B2's.
- **Firmware path**: bootargs `firmware_class.path=/data/firmware` (searched before `/lib/firmware`); the driver's
  size and SHA-256 pins unchanged. `/data` (Y2DATA) is mounted at about 2.3 s; the attempt runs only on request.
- **Variant thirty-third**: `kernel/config/y1-thirty-third-boot.config` (the thirty-second fragment, LOCALVERSION
  `-y1-thirty-third-boot`), `kernel/dts/innioasis-y1-thirty-third-boot.dts` (the thirty-second tree with the `wmt`
  mode, `ldo_vcn33_bt`/`ldo_vcn33_wifi` fixed at 3.3 V without always-on/boot-on, the firmware path),
  `y1-boot.sh`, `y1_boot.py` (regulators: vgp1, vusb, card rails, vcn18, vcn28, vcn33-bt, vcn33-wifi; `chip-id`
  forbidden). The two VCN33 LDOs share VOSEL `0x416` [3:2], which LK leaves at 3 (3.6 V): with 3.3 V fixed the
  regulator core writes it to 0 at boot, both rails off (stock's `hwPowerOn(…, 3300)` would). Build
  `out/y1-thirty-third-boot-1`: BOOTIMG **`b1baff35662c0d826fa9b081ee61527139ae15dc601879b94b29d39980ac7929`**,
  `0x384000` bytes (3 686 400), zImage-dtb `45f01610…dcf5`, DTB `285c512a…eb62`, `.config` `8600185b…6ec0`,
  `y1-init` unchanged; no compiler warning (the driver also clean with `-Werror` and `W=1`), only the fork's known
  DT warnings.
- **Tests** (`tests/test_y1_connectivity.py`, 19, sandbox): the B4 model now shared; the B5 attempt runs the driver's
  real `bring_up_wmt`, stages, power path and `calibrate` against the register model with a WMT model: order (B4
  power-on, BTIF only after the chip ID, every WMT command with BTIF and the domain up, the calibration only inside
  the PA window, both PA rails off before the sleep command, sleep before BTIF stop, protection, rails off); a
  refused patch, a refused calibration, an abort during the calibration and a pending abort: each back at the
  baseline and retryable; any calibration length accepted, a bad status, operation or inner length refused; the
  shared `y2_btif_start` writes no register before its two clocks and enables interrupts last.

**Procedure** (owner, over SSH; the Y1 on the thirty-third boot; the device path is
`/sys/devices/platform/18070000.connectivity`):

1. Copy the owner's provisioned files (`tools/connectivity/provision.py --profile y1` output) and check them:

       ssh root@<y1> 'mkdir -p /data/firmware/mediatek/mt6572'
       scp WMT_SOC.cfg mt6572_82_patch_e1_0_hdr.bin modem_1_2g_n.img root@<y1>:/data/firmware/mediatek/mt6572/
       ssh root@<y1> 'cd /data/firmware/mediatek/mt6572 && sha256sum * && sync'

   Expected: `161f18b6…f1e0`, `5c13ebad…bfc1`, `31940b2f…4b79` (the inventory; the modem image is not used).
2. Baseline read-only (as B4) and `dmesg | grep 18070000` shows `bring-up wmt: ready (bring_up runs one attempt,
   power_off aborts)`; `cat …/status` shows `powered=0`.
3. Trigger (blocks until the attempt ends, about 6–10 s the first time, 20 s at most plus 4 s per silent command):

       ssh root@<y1> 'echo 1 > /sys/devices/platform/18070000.connectivity/bring_up; echo rc=$?; dmesg | grep 18070000'

4. Abort, from a second session at any time: `echo 1 > /sys/devices/platform/18070000.connectivity/power_off`
   (logs `connectivity failure -125` at critical level, then `power-off on request: 0`).

**Expected log** (prefix `y1-connectivity 18070000.connectivity: `; `…` = observed values):

    bring-up wmt: attempt
    PMIC init subset 0: 0x102 0003->8043 0x120 0001->0031 0x148 0004->000e 0x402 8400->8401
    stage normalise / stage vcn18 / stage vcn28 / stage emi-clear / stage spm-clock / stage domain-on / stage chip-id
    CONN chip=6572 (register 0x00006572)
    stage btif
    stage wmt
    WMT_SOC.cfg: the Y1 key set
    WMT opcode=08 parameter=02 ok: event 16 bytes                       (three times: chip, HVR, FVR)
    CONSYS chip=6572 HVR=8a0… FVR=…
    WMT opcode=04 parameter=04 ok: event 10 bytes                       (query, mandatory STP)
    WMT opcode=04 parameter=03 ok: event 6 bytes                        (set full STP)
    WMT opcode=04 parameter=04 ok: event 10 bytes
    WMT full STP mode verified
    WMT opcode=08 parameter=01 ok: event 8 bytes                        (twice: PATCH_ADDRESS, P_ADDRESS)
    WMT opcode=07 parameter=04 ok: event 5 bytes                        (reset)
    WMT patch: 39868 bytes, 40 fragments, address 00 00 06 00; reset acknowledged
    WMT RF calibration: VCN33_BT and VCN33_WIFI on
    WMT RF calibration: event … bytes, status 00 operation 01: 0
    WMT RF calibration: PA rails off (0, 0)
    WMT opcode=10 parameter=01 ok: event 5 bytes                        (coexistence)
    WMT opcode=03 parameter=01 ok: event 6 bytes                        (sleep)
    bring-up wmt: WMT sleep 0
    bring-up wmt: off: ctl=0x1112 status=0x3ffd sleep=0x… protect=0x11
    bring-up wmt: done 0: chip=6572 HVR=… FVR=… calibration=… bytes status=0001

`rc=0`. A failure shows `WMT opcode=… failed: …` (or `STP receive failed`, `WMT patch failed`, `WMT_SOC.cfg
rejected/unavailable`), `connectivity stage … failed: … (power-off 0)`, the `off:` line, `bring-up wmt: failed …`
and a non-zero `rc`. `CONN did not sleep within 10 s; isolating` means the sleep command did not take (B5 finding).
Record HVR, FVR, the calibration length and status, and every line in `evidence-private/2026-11-06-y1-bt-b5/`.

**Post-run read-only checks** (never `0x18070000` or BTIF/DMA): SPM `0x10006280` `0x1112`; `0x1000660c`/`0x10006610`
`0x3ffd`; MD1 `0x10006284` `0x0d`; `0x100063b4` bit 11 clear; INFRA `0x10001220` `0x11`, `0x10001228` `0xf011`;
TOPCKGEN `0x10000024` `0xd8cfdaff` (BTIF and APDMA gated again); PMIC `0x512` `0000`, `0x41c` `0000`, `0x418`
`0000`, `0x420` `0000`, `0x41a` `0044`, `0x416` `0010` (VOSEL 3.3 V since boot, bits 5 and 7 clear); the init
subset **stays applied** (`0x102` `8043`, `0x120` `0031`, `0x148` `000e`, `0x402` `8401`, as stock runs); `status`
`powered=0`; regulators vcn18, vcn28, vcn33-bt, vcn33-wifi disabled; `/sys/class/bluetooth/` empty; the device
still bound. Then the usual regression check (splash, Reborn, MTP, Y2DATA, charging) and a second attempt to show
it repeats (the subset is then skipped).


## 13. B5 run — 2026-11-06

`evidence-private/2026-11-06-y1-bt-b5/run1.txt`, thirty-third boot. The owner's three files copied to
`/data/firmware/mediatek/mt6572/` (SHA-256 identical to the inventory); `firmware_class.path=/data/firmware`. Two
attempts over SSH, identical:

- PMIC init subset (first attempt only; the Y2's 5 s settle): `0x102 0003→8043`, `0x120 0001→0031`, `0x148
  0004→000e`, `0x402 8400→8401`; the B4 stages; `CONN chip=6572`; BTIF; WMT with the Y1 key set.
- **`CONSYS chip=6572 HVR=8a01 FVR=8a00`**; full STP verified; **the patch: 39 868 bytes, 40 fragments, address
  `00 00 06 00`, reset acknowledged**; **RF calibration: event 630 bytes, status 00, operation 01**, the PA rails on
  for 0.3 s only; coexistence; **WMT sleep accepted and the core slept** (no 10 s wait). **No modem: B7 not needed.**
- **The power-off's bus-protection acknowledgement times out** (`CONN off step 2 (reg 0x228) failed: -110`) after
  the core slept; the rest of the power-off runs (`ctl=0x1112 status=0x3ffd`), the attempt reports `failed -110`.
  `0x10001228` reads `0xf000` afterwards (B4, without sleep: `0xf011`), `0x10001220` `0x11`: a sleeping core does
  not acknowledge the protection request (INFERRED). Open: what stock does there (B5 fix).
- Afterwards: SPM CONN `0x1112`, `PWR_STATUS` `0x3ffd`, MD1 `0x0d`, TOPCKGEN `0xd8cfdaff`, the PMIC init subset values
  as above, `0x416` `0x0010`, `0x512`/`0x41c`/`0x418`/`0x420` `0000`, the four rails disabled, `calibrated=1`; Reborn and
  MTP running.

### 13.1 B5 fix — 2026-11-06 (offline, built, not flashed)

- **Stock, decoded** (3.0.2 kernel, `evidence-private/2026-11-04-y1-rom-3.0.2/rea/text.dis`):
  - `conn_power_off` (`0xc003f73c`) first waits for `spm_is_conn_sleep` (`0xc002e53c`), polling `msleep(100)` up
    to `(timeout+9)/10` times (`0xc003f790`–`0xc003f7d4`). With or without sleep it then calls `disable_subsys_locked`
    under the clock lock (`0xc003f7f4`); a non-zero result only WARNs (`0xc003f824`).
  - `spm_mtcmos_ctrl_general_locked`, off path: if the subsystem has a protection mask (`[r0+0x24]`, `0x11` for
    CONN), it sets `0x10001220 |= mask` (`0xc00407c4`–`0xc00407dc`). It then polls `(0x10001228 & mask) == mask`
    until `jiffies + 10` (100 ms; `0xc00407ec`–`0xc0040808`).
  - **On a timeout it WARNs (`0xc0040b28`, source line `0xc65`) and goes on with the isolation** (`b 0xc004080c`).
    The final `0x60c`/`0x610` poll is handled the same way (`0xc0040948`).
  - So stock's order is sleep handshake, protection, acknowledgement poll, isolation, and it never stops on a
    missing acknowledgement.
  - The idle guarantee there is the core's sleep handshake, together with the AP side already quiet (BTIF/DMA
    stopped before `conn_power_off`).
- **Driver**:
  - `domain_off` logs the acknowledgement state at info level, e.g. `CONN bus protection: 0x228=0xf000, not
    acknowledged (core asleep: continuing, as stock)`.
  - With the core asleep (SPM `0x100063b4` bit 11 clear after the sleep wait), a missing acknowledgement is not an
    error. The B5 attempt then reports `done 0`.
  - Without the handshake a missing acknowledgement stays an error. The isolation and the rails still switch off,
    as stock.
  - Power is never cut while the core is awake and unacknowledged without that error being reported.
- **PMIC subset settle**: stock applies `PMIC_INIT_SETTING_V1` (`0xc03da084`) once in its PMIC init (call
  `0xc03da760`). It continues straight to `PMIC_EINT_SETTING` (`0xc03da774`) with no delay. The driver now applies
  the subset at probe in the `wmt` and full modes, logged; the attempt no longer waits. The Y2's 5 s settle is
  dropped.
- **Tests** (`tests/test_y1_connectivity.py`, 19): the register model can withhold the `0x228` acknowledgement.
  - With the core asleep, the chip-id bring-up and the WMT attempt succeed (630-byte calibration, as run 1), back
    at the baseline.
  - With the core awake (sleep refused), they power off completely and report `-ETIMEDOUT`.
- **Build** `out/y1-thirty-third-boot-2`, same fragment and tree, only `core.c` changed:
  - BOOTIMG **`327f577e0221c204b61110efcacf3e494ca4b8063c66438cd370e51063ca7171`**, `0x383800` bytes (3 684 352).
  - zImage-dtb `48765c16…16c0`; DTB `285c512a…eb62` and `.config` `8600185b…6ec0` unchanged.
  - No compiler warning.
- **Expected change in the run log**:
  - `PMIC init subset 0: …` now appears at boot after `bring-up wmt: ready`, not in the attempt.
  - The power-off shows `CONN bus protection: 0x228=0xf000, not acknowledged (core asleep: continuing, as stock)`
    instead of `CONN off step 2 … failed: -110`.
  - The attempt ends with `bring-up wmt: done 0: … calibration=630 bytes status=0001`.
- **Expected change in the post-run checks**: none (`0x10001228` `0xf000`, the rest as §13).

**B5 run 2 — PASS — 2026-11-06** (`evidence-private/2026-11-06-y1-bt-b5/run2.txt`, `out/y1-thirty-third-boot-2`,
`327f577e…`): the PMIC subset at probe (2.4 s, no settle); two attempts, each about 1.05 s: `HVR=8a01 FVR=8a00`, the
patch (40 fragments, reset acknowledged), calibration 630 bytes status 00, WMT sleep, `CONN bus protection:
0x228=0xf000, not acknowledged (core asleep: continuing, as stock)`, `off: ctl=0x1112 status=0x3ffd`, **`done 0`**,
`rc=0`. Afterwards CONN `0x1112`, `PWR_STATUS` `0x3ffd`, `0x10001220` `0x11`, `0x10001228` `0xf000`, `powered=0`,
`calibrated=1`.

## 14. B6 build — 2026-11-06

Owner-approved after the B5 run. hci0 as on the Y2, the owner's own address record, BlueZ, BlueALSA and Reborn.
Nothing flashed or sent to the Y1; the record is in no repository file.

- **The address record**:
  - **Source:** the owner's backup (`evidence-private/2026-11-04-y1-backup/read1/emmc_user.bin`). USRDATA is at logical
    `0x4d540000` (ext4 magic checked there). The record is `/nvram/APCFG/APRDEB/BT_Addr`: 32 bytes, written
    2022-08-01.
  - **What it holds:** address `xx:xx:xx:xx:72:75`, unicast and locally administered. It is **not** the generic
    MT6572 default `00:00:46:65:72:01` (stock generated it once). The radio, sleep and TX-power bytes 6–29 equal
    stock's defaults; bytes 30–31 are unused.
  - **Tool:** `tools/connectivity/y1_bt_record.py` reads it with debugfs straight from the dump (`image?offset=`, no
    partition copy). It applies the driver's rule, writes a 0600 file and prints only the last two bytes. The
    driver checks the record again on the `factory` write and refuses a second write.
- **Kernel** (`kernel/y1/connectivity/core.c`): the full mode (no `innioasis,bring-up`) is the Y2 driver's model.
  - **Probe:** checks the wiring, claims everything, applies the PMIC subset (as stock's PMIC init) and exposes
    `factory` (32 bytes, write-only), `activate`, `status` and `recover`. `bring_up`/`power_off` exist only in the
    `wmt` mode.
  - **`activate`:** registers hci0 and powers nothing.
  - **hci0 up** (BlueZ powering the adapter): runs the B5 boot, then WMT BT function on and the VCN33_BT rail on.
  - **hci0 setup** (shared `hci.c`): reads the controller address. If it is the generic default or invalid, the
    record's address is used. The commands are `0xfc1a` (address, reversed), `0xfc79` (record 12–17), `0xfc93`
    (27–29), `0xfc7a` (18–24), then Reset; these are stock's `bt_init_script_6572` offsets and order.
  - **hci0 down:** function off and PA rail off. After the 1 s autosuspend: **WMT sleep, then the power-off** (new
    `sleep_then_off`, also used at shutdown), so the core sleeps as in B5 run 2. A failed transport powers off
    without the sleep command.
- **Userspace** (Y2ROOT unchanged; Y1 overrides in `initramfs/y1-bluetooth/`, PID1 `Y1_THIRTY_FOURTH_BOOT`, list
  `y1-thirty-fourth-boot.list`):
  - **Service list:** gains the Y2's `S30dbus-daemon` and `S41y2-connectivity`. S40bluetoothd is left out because
    y2-radiod starts bluetoothd (and `bluealsad -p a2dp-source`) itself.
  - **The Y1's `S41y2-connectivity`** (it replaces the Y2's; the power daemon's shutdown calls its
    `stop-wifi`/`stop-bluetooth`/`stop-daemon`):
    1. the Y2's `/data/bluetooth` layout and the `/var/lib/bluetooth` and `/var/lib/bluealsa` binds, so pairings live
       on Y2DATA;
    2. seeds `enabled` = 0 and `codec-policy.json` with `experimental:false` if they are missing;
    3. checks `/data/connectivity/bt-record` (root, 0600, 32 bytes), writes it to `factory`, then `activate`;
    4. starts `y2-radiod`.
  - **Not run on the Y1:** y2-factory, y2-calibration, wpa_supplicant and connectivity-daemon (MT6582 or Wi-Fi only).
    radiod needs no Wi-Fi: with no wlan rfkill entry, Wi-Fi simply reads as off.
  - Reborn's Bluetooth screens and its `bluealsa:DEV=…,PROFILE=a2dp` output are unchanged.
- **Kernel config:** the thirty-third fragment plus the Y2's remaining Bluetooth lines: `RFKILL`, `BT_RFCOMM` (TTY,
  HIDP and BNEP off), `HID_SUPPORT`, `HID`, `UHID` and `CRYPTO_SHA256/AES/CCM` (SMP's `cmac(aes)` needs AES). The
  initramfs is the new list. Wi-Fi stays off.
- **Proof the Y2 is unchanged:** no Y2 file changed in B6. The Y2 production rebuild (`out/b6-y2-after`) is again
  `Image` `a68fdffd…aac21`, `vmlinux` `b5ae438d…eb202` and the same seven objects. The thirtieth PID1 rebuilt with
  its own defines is still `699cd02e…`.
- **Build** `out/y1-thirty-fourth-boot-1`:
  - BOOTIMG **`9f82ecf35390ada2c98cae082542259a63fac8c84085a333358a4c876ccc9fcc`**, `0x39c800` bytes (3 786 752).
  - zImage-dtb `a36e2e37…f1ed`, DTB `7aed6b94…c23a`, `y1-init` `828f272e…c676`, `.config` `ddd1ff8b…218d`.
  - No compiler warning; the driver is also clean with `-Werror` and `W=1`.
- **Tests:**
  - `tests/test_y1_connectivity.py` (23):
    - the shared `hci.c` `setup` with a synthetic record: the command order, lengths and record offsets, the reversed
      address, the default rejected in favour of the record, a valid controller identity preferred, and the
      identity chosen once;
    - the factory rule;
    - the actual open/close sequence (power-on, PA, function on … function off, PA off, WMT sleep, power-off; no
      sleep after a failure; nothing without activation);
    - the B6 service files.
  - `tests/test_y1_bt_record.py` (2): the rule, and an extraction from a synthetic dump at an offset (needs
    e2fsprogs; it ran on the host).

**Owner procedure** (`<y1>` = its USB-network address; the B5 firmware stays in `/data/firmware/mediatek/mt6572/`):

1. On the host, extract into a private directory outside the repository:

       python3 tools/connectivity/y1_bt_record.py \
           --backup evidence-private/2026-11-04-y1-backup/read1/emmc_user.bin --output ~/y1-private

   Expected: `bt-record: 32 bytes, address xx:xx:xx:xx:72:75, written 0600 …`.
2. Flash the thirty-fourth BOOTIMG (owner), boot, then copy the record:

       ssh root@<y1> 'mkdir -p -m 700 /data/connectivity'
       scp ~/y1-private/bt-record root@<y1>:/data/connectivity/bt-record
       ssh root@<y1> 'chown root:root /data/connectivity/bt-record && chmod 600 /data/connectivity/bt-record && sync && /etc/init.d/S41y2-connectivity start'

   From the next boot on, S41 does this itself.
3. Expected `dmesg | grep -i '18070000\|hci0\|Bluetooth'`:
   - at boot: `PMIC init subset 0: …` and `MT6572 connectivity owner ready (Bluetooth); radio awaits the factory record`;
   - after activation: hci0 registered, `status` = `activated=1 powered=0`;
   - on enabling Bluetooth in Reborn:
     - `CONN chip=6572 (register 0x00006572)`;
     - the B5 WMT lines (`CONSYS chip=6572 HVR=8a01 FVR=8a00`, the patch line, `RF calibration: event 630 bytes,
       status 00 operation 01: 0`, coexistence);
     - `WMT opcode=06 parameter=00 ok` (function on);
     - `Bluetooth identity: persistent per-device Y2DATA fallback` (the controller reports the default) or `valid
       controller identity`;
   - on disabling: `WMT opcode=06 …` (off), `WMT opcode=03 … ok`, the protection line, `status` `powered=0`.
4. Tests (SSH; there is no `hciconfig` in Y2ROOT):
   - `cat /sys/devices/platform/18070000.connectivity/status`.
   - `bluetoothctl show`: the address ends in `72:75` (when the controller reports the default), Powered: yes once
     enabled in Reborn.
   - `bluetoothctl --timeout 20 scan on` lists nearby devices.
   - In Reborn: enable Bluetooth, pair the headphones, play music. `bluealsactl list-pcms` (or `ps` for `bluealsad`)
     shows the A2DP PCM.
   - Pause/next on the headphones.
   - Disable Bluetooth: `powered=0`, all four rails off, SPM `0x10006280` = `0x1112`, `PWR_STATUS` bit 1 clear.
   - Power off from Reborn, boot again: hci0 is activated by S41, `bluetoothctl devices` still lists the headphones
     (pairing on Y2DATA), and they reconnect from Reborn.
   - Keep `dmesg`, `/run/y2/radiod-state.json` and
     `/data/system/platform/bluetooth-journal.jsonl` into `evidence-private/2026-11-06-y1-bt-b6/`.

**Owner decisions:**
1. The identity: the stock record's address (recommended, the unit's own since 2022). The shared Y2 rule prefers a
   valid controller address when the controller reports one that is not the default.
2. Codecs: SBC plus whatever the build's inventory qualifies (AAC is eligible only when qualified and without CPU
   pressure). Reborn's codec preference "SBC" pins SBC on the MT6572.
3. The PMIC subset at probe in the full mode (stock's PMIC init applies it at every boot).

## 15. B6 run 1 and the extended-features fix — 2026-11-06

**Run 1** (`evidence-private/2026-11-06-y1-bt-b6/run1.txt`, BOOTIMG `9f82ecf3…`): activation, the B5 boot, the
calibration (630 bytes, status 00), function on and the record's identity all as expected. Then BlueZ's init failed
with **`hci0: Opcode 0x1004 failed: -38`** (Read Local Extended Features), and the adapter went down cleanly
(function off, WMT sleep, power-off; `status` `powered=0 error=0 transport_errors=0`).

**Cause:** the firmware's HVR `8a01`/FVR `8a00` is the one the Y2 already traced (E2): page 1 reports `max_page=2`,
and page 2 is rejected with status `0x30`. `bt_to_errno` has no entry for `0x30`, so that becomes `ENOSYS` (-38). The
Y2's `controller_quirks` sets `HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2`, but only for chip `0x6582`.

**Fix:** the chip in that check is now `Y2_CONN_EXT_FEATURES_QUIRK_CHIP`, defined per SoC in `conn.h` (`0x6582` in
the Y2's, `0x6572` in the Y1's). Nothing else changes: same HVR/FVR condition, same quirk, `max_page` 1.

- **Tests:** the shared `setup` test now runs with the Y1's chip and HVR/FVR, and checks the quirk and `max_page==1`
  (`tests/test_y1_connectivity.py`; 25 run, 1 skipped, in the build userspace).
- **Build** `out/y1-thirty-fourth-boot-3`: BOOTIMG **`727315aa2eaa09256bcb366e7a087d1416ccca14e2b6ba2b75e7da331325bb49`**,
  `0x39c800` bytes, zImage-dtb `da509978…43ae`. DTB, `y1-init` and `.config` are identical to build 1. No compiler
  warning. (`out/y1-thirty-fourth-boot-2` was interrupted and is not a candidate.)
- **Owner procedure:** unchanged from §14 (the record is already in `/data/connectivity/`). Expected after enabling
  in Reborn: no `0x1004` failure, and `bluetoothctl show` reports `Powered: yes`.

**Run 3** (BOOTIMG `727315aa…` read back from physical `0x2b80000`; `run3-boot.txt`): `hci0: broken local ext
features page 2`, then `MGMT ver 1.23`. BlueZ lists the controller with the record's address (`…72:75`). With
`y2-radiod ctl bluetooth on`: `Powered: yes`, `powered=1 functions=0x1`, `vcn33-bt` on at 3.3 V. Inquiry finds the
host's adapter and the owner's headset. A bare `bluetoothctl pair` reported `Pairing successful`, but no bond was kept
(the adapter was not pairable); see below for the next attempt.

**Reborn's "Bluetooth Needs the Radio Firmware":** `y2-platform firmware status` (`radiofw::files::present`) checks
the Y2's five MT6582 files plus two defaults in `/data/firmware/mt6582` by name and size only. The Y1 has none of
them; its radio runs on the MT6572 set in `/data/firmware/mediatek/mt6572`. **Owner workaround (2026-11-06, on the
Y1's Y2DATA only):** seven zero-filled sparse placeholders with the reviewed names and sizes in `/data/firmware/mt6582`
and a note in `/data/firmware/Y1-MT6582-PLACEHOLDERS.txt`; the status is then `Ready`. Nothing on the Y1 reads their
bytes: the Y1 driver loads the MT6572 set, `y2-factory` (the only reader of `/lib/firmware/mediatek/mt6582`) is not
run, and the hash-checking scan/install runs only while the state is `Missing`. Still open: a Y1-aware firmware
state in the platform.

## 16. B7 plan: radio firmware from the owner's copy — 2026-11-06

Owner-approved, roadmap audit first (`roadmap-gap-audit.md`, "Y1 Bluetooth B7"). Goal: a fresh Y1 gets Bluetooth
the way a Y2 does (RELEASE-49). The owner copies `system.img` from the Y1's firmware package into a `Firmware`
folder over USB, and the player checks it, installs the radio files, deletes the copy and starts Bluetooth. No
hand-copied files, no extracted record, no placeholders.

**The source** (verified offline): the public Y1 3.0.2 `rom.zip` (`7ee9aa5b…`, the same file as the 2026-11-04
evidence) contains `system.img`, a raw ext4 image of 681 574 400 bytes (`5b617698…`, the ANDROID partition's size):

| In `system.img` | Bytes | SHA-256 | Use |
| --- | --- | --- | --- |
| `/etc/firmware/WMT_SOC.cfg` | 82 | `161f18b6…f1e0` | as pinned since B0 |
| `/etc/firmware/mt6572_82_patch_e1_0_hdr.bin` | 39 868 | `5c13ebad…bfc1` | as pinned; header byte 24 `0x11` (one part) |
| `/lib/libbluetooth_mtk.so` | 13 452 | `36f2ea80…afd7` | the stock default `BT_Addr` record, once, at file `0x3004` |

The library's record is the generic address `00:00:46:65:72:01` and then bytes 6–29 equal to the owner's own
record (bytes 30–31, unused, differ). The stock record itself exists only in stock USRDATA (`/nvram`). It is in no
raw partition (NVRAM, PROTECT_F/S searched in the backup), so an installed Y1 can no longer read it.

**Design** (owner decisions, 2026-11-06/07: the platform's own import, configured by the image rather than by
detecting the board; folder `/data/firmware/mt6572`, named like the Y2's `/data/firmware/mt6582`; a random
per-device address when no stock record is supplied; **no Reborn change**, so on the Y1 Reborn's firmware page
still names the Y2 package and the "Firmware drive"):

1. **Platform** (`y2-platform firmware`, `radiofw`): an image may carry `/etc/y2linux/radio-firmware.json`. It
   lists the target folder, the copy's folder and its note, the files of the stock `/etc/firmware` (name, size,
   SHA-256), the stock library, the default records cut from it (offset, size, the expected leading bytes, the
   bytes to clear, SHA-256) and the patch order. Without the file the Y2's built-in tables apply, byte for byte as
   before. A file that is there but invalid is `Failed` (`radio_firmware_config`), never the Y2's tables. The root
   is read-only, so the image vouches for the file like its other `/etc/y2linux` files.
2. **Y1 image** (BOOTIMG overlay): `initramfs/y1-bluetooth/etc/y2linux/radio-firmware.json` with the two files,
   `/lib/libbluetooth_mtk.so` and `BT.defaults` (`0x3004`, 32 bytes, `000046657201` expected, 6 cleared), patch
   order `[1]`, inbox `/data/music/Firmware` (Music, the Y1's one MTP storage), installed by PID1 like the Y1's
   other `/etc/y2linux` overrides. The driver loads `mt6572/…` (`Y2_CONN_FW`), so from `/data/firmware/mt6572/`.
   The Y1's `S41y2-connectivity`:
   - moves the B5/B6 hand-copied `/data/firmware/mediatek/mt6572` to `/data/firmware/mt6572` once;
   - while `firmware status` is not `Ready`, runs `y2-platform firmware scan` every 5 s in the background (the Y1
     has no Y2 service daemon); the scan restarts S41 itself after an install;
   - if `/data/connectivity/bt-record` is missing, composes it from `BT.defaults` and a new random address:
     unicast, locally administered, from `/dev/urandom`, root 0600, written once. An owner-supplied stock record
     always wins;
   - then activates hci0 as in B6.
3. **Delivery**: the BOOTIMG part works with any Y2ROOT. The platform part needs a new Y2ROOT on the Y1; until
   then the B6 workaround (placeholders) stays on the owner's Y1.

## 17. B7 build — 2026-11-07

- **Platform** (`radiofw.rs`, `radiofw/files.rs`, `radiofw/copy.rs`): `Config`/`Entry`/`Slice` (serde,
  `deny_unknown_fields`) and `Reviewed::of(root)`. Validation: schema 1, the target under `/data/firmware/`, absolute
  paths without `..`, plain file names, 64-digit hashes, slices that fit. `Reviewed` gains the slices, the patch
  order, the target, the inbox and the note; `sliced()` cuts the defaults. The staging names come from the target;
  the library's name is in its mismatch reason. No contract change: `firmware status` answers as before.
- **Tests**:
  - platform: 4 new `radiofw` tests (no config keeps the Y2 tables; a set is read and nine bad ones refused, each
    `Failed`/not present; the slices need the stock layout; a full scan install from a synthetic raw-ext4 image
    through a config, into `/data/firmware/mt6572`, the configured note and folder gone afterwards), 15/15. The
    whole platform suite passes. The golden suites fail exactly as at `HEAD` (the 20 pre-existing differential
    failures, the 4 reports failures; `boot-evidence-rotation` is flaky on both).
  - The Y1's config was checked against the real stock files (sizes, hashes, the expected bytes, the derived
    `BT.defaults` hash) from the public `rom.zip`.
  - Y1: `test_y1_connectivity` 28 (1 skipped) in the build userspace: the config equals the firmware inventory,
    PID1 and the list install it, `Y1RecordB7`. `compose()` was also run with the Y1's BusyBox in a temporary
    folder: 0600, settings kept, first octet `0x4a`.
  - The code-quality ratchet passes.
- **Build** `out/y1-thirty-fourth-boot-5`: BOOTIMG **`01f78b589490fea3f417c2bbe7c9f412dde6a11848c7c548e36f5c87af35ab0d`**,
  zImage-dtb `b845699f…8234`, `y1-init` `d278e15b…5ba4` (the new override); DTB and `.config` as build 3. Build 4
  (`be0596e2…`) was the board-detection variant, superseded and never flashed.
- **Owner procedure:**
  1. Flash build 5 now; with today's Y2ROOT Bluetooth keeps working (the folder is moved once, the record kept;
     the old platform ignores the config and still sees the B6 placeholders as `Ready`).
  2. The import itself arrives with the next Y2ROOT. Then:
     - remove the B6 placeholders (`/data/firmware/mt6582`, `/data/firmware/Y1-MT6582-PLACEHOLDERS.txt`);
     - `firmware status` answers `Missing`. Reborn names the Y2 package (owner decision), but
       `Music/Firmware/README.txt` names the Y1's;
     - copy `system.img` from the Y1 3.0.2 `rom.zip` into `Music/Firmware`. It is installed within about 10 s and
       the copy deleted. `firmware status` answers `{"state":"Ready"}`, and Bluetooth works with the owner's
       stock record kept;
     - a fresh Y1 without a record gets a random address (`compose`).
- **Not covered:** a second `system.img` copied after the install stays until deleted (on the Y2 the service
  daemon's `leftover` removes it; the Y1 runs no scan once Ready).
