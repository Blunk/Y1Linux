# Y1 Bluetooth

Status: **B0, B1 done; B2 built, not loaded** (2026-11-06, owner-approved B0–B2; B3 not started). The owner asked whether the Y1 can get Bluetooth the way the Y2 did
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

