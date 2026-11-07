# Y1 Wi-Fi

Status: **W0–W2 done 2026-11-07** (owner-approved): the firmware source decided (F2, the owner's Y2 file, by hash
comparison, §2), the Y1's Wi-Fi record found (§7), the driver extended and built, not loaded (§8); **W3 PASS**
(§9–§10: the Wi-Fi firmware boots on the Y1, `wlan0` registered); **W4 PASS** (§11–§12: passive and active scans
find networks: the Y1's Wi-Fi receives and transmits); **W5 PASS, throughput open** (§13: WPA2 association,
DHCP, DNS, Internet and intact transfers on the owner's network, but 1–8 Mbit/s); **W6 built** (§14, owner-approved
before W5b: Wi-Fi as a product, the Y2's connectivity service and Reborn's screens, the firmware imported from the
two packages; its userspace PASS live on build 3, BOOTIMG build 5 PASS on the Y1 with the late Wi-Fi record, the
platform part with the next Y2ROOT); W5b open. The Y1 ships with Wi-Fi disabled and no Wi-Fi firmware. New hardware scope (a
radio function, a transmitter, firmware not taken from this device): each stage has its own admission, and the
roadmap audit is repeated before W3 (the first Wi-Fi power-on).

## 1. Evidence

Offline, read-only: the owner's eMMC backup (`evidence-private/2026-11-04-y1-backup`), its `ANDROID` system
partition (`ANDROID-from-backup.img`, debugfs), the 3.0.2 ROM analysis (`evidence-private/2026-11-04-y1-rom-3.0.2`),
MediaTek's public MT6572 kernel drop (`drsami/mt6572_kernel`), and the Y2's driver. CONFIRMED unless marked.

- **Innioasis built the Y1 without Wi-Fi.** The device's own copy of the vendor build configuration (in the eMMC
  dump at byte 206 550 416) says `MTK_WLAN_SUPPORT = no`, `MTK_WIFI_HOTSPOT_SUPPORT = no`,
  `MTK_WIFI_P2P_SUPPORT = no`, `MTK_WLAN_CHIP = MT6572`, `MTK_COMBO_CHIP = MT6572_CONSYS`, `CUSTOM_HAL_COMBO =
  mt6572_82`, `CUSTOM_HAL_ANT = mt6582_ant_m1`, `MTK_WLANBT_SINGLEANT = no` (and `MTK_BT_SUPPORT = yes`,
  `MTK_FM_SUPPORT = yes`, `MTK_GPS_SUPPORT = no`). MediaTek's MT6572 reference `ProjectConfig.mk` has
  `MTK_WLAN_SUPPORT = yes`: Innioasis switched it off.
- **No Wi-Fi firmware or driver anywhere on the device.** System `/etc/firmware`: `WMT_SOC.cfg`, the Bluetooth
  patch, the modem image, `catcher_filter_1_2g_n.bin`, `S_ANDRO_SFL.ini` and `mt6627/` (FM only). No
  `WIFI_RAM_CODE*`, no `wlan*.ko`, no `wlan_loader`. The string `WIFI_RAM_CODE` occurs nowhere in the whole eMMC
  dump. The stock kernel has no WLAN driver built in (kallsyms: only the board hooks `mt_wifi_power_on/off`,
  `dct_pmic_VCN33_enable_wifi`, `upmu_set_*_vcn33_*_wifi`, and cfg80211's `nl80211`). Stock init still loads
  `mtk_wmt_wifi.ko` (5444 bytes, the `/dev/wmtWifi` character device that asks WMT to switch the Wi-Fi function)
  and names `wlan_loader`, which is absent: template leftovers. `wpa_supplicant` is AOSP's.
- **The silicon has a Wi-Fi core.** MT6572 CONSYS is the same family as the Y2's MT6582 (B0, B1): the shared
  `mt6572_82` patch family, HW/FW version `8a00` on both. In MediaTek's MT6572 kernel drop
  (`mediatek/platform/mt6572/kernel/core/include/mach/`):

  | Item | Y2 (MT6582, in use) | Y1 (MT6572) |
  | --- | --- | --- |
  | Wi-Fi HIF window | `0x180f0000`, 0x5c | `CONN_HIF_BASE 0xF80F0000` (virtual) = **`0x180f0000`** (same mapping as `CONN_MCU_CONFIG_BASE 0xF8070000` = `0x18070000`, CONFIRMED on the Y1 in B4) |
  | Wi-Fi HIF interrupt | SPI 184 | `MT_WF_HIF_IRQ_ID` = **SPI 123** |
  | CONN watchdog | — | `MT_CONN_WDT_IRQ_ID` = SPI 124 |
  | BTIF / wake | SPI 50 / 185 | SPI 52 / 122 (matches B1 and the running Bluetooth) |

- **MediaTek's driver for the MT6572 Wi-Fi core is the "mt6582" driver** (checked 2026-11-07 in the drop,
  commit `c4b775c` "Initial source from Acer", `mediatek/kernel/drivers/conn_soc/drv_wlan/mt6582/wlan`, read only).
  This is an MT6572 BSP building that driver for the MT6572, with explicit MT6572 handling:
  - `os/linux/hif/ahb/include/hif.h`: `MTK_CHIP_ID_6572 0x6572` and `MTK_CHIP_ID_6582 0X6582`;
    `CONN_MCU_CONFIG_BASE 0xF8070000 //MT6572`.
  - `include/nic/mt6582_reg.h`: `MTK_CHIP_REV 0x00006572` (the expected `WCIR` chip ID).
  - `os/linux/hif/ahb/ahb.c`: "Port sdio.c to ahb.c on MT6572/MT6582"; GDMA only `if (ChipID ==
    MTK_CHIP_ID_6572)` and only with `CONF_MTK_AHB_DMA` (DMA off by default, `fgDmaEnable = FALSE`); the Makefile
    takes that DMA code from `platform/mt6572/kernel/drivers/wifi/`.
  - `include/config.h`: `CFG_6572_SPM_WORKAROUND 1` ("suspend current is higher than deep idle"; an AP-mode
    wake lock in `gl_init.c`, `gl_p2p_cfg80211.c`, `adapter.h`). The Y2's copy has it as
    `CFG_SPM_WORKAROUND_FOR_HOTSPOT 0`; it matters only for hotspot and suspend, not for station mode.
  - Driver version `2.0.1.1` (`os/version.h`), the same as the Y2's copy. None of the Y2 donor's 122 files is
    byte-identical (the donor is a later revision: it adds `hs20.c` and `mtreg.h`). The samples compared
    (`nic.c` and others) differ by licence headers, types, format strings and helpers, not by chip logic.
  - **The Y2's copy already accepts the MT6572**: `wifi/nic/nic.c:1260` lets `MTK_CHIP_REV_72` (`0x6572`) and
    `_82` through, from `wifi/include/nic/mtreg.h:199-201` (72/82/92). Only `wifi/hif.h` names 6582 alone, and only
    for the glue's display string.
- **Turning Wi-Fi on** (the drop's `common/core/wmt_func.c`, `wmt_ic_soc.c`): on the SoC there is no WMT
  command. `wmt_func_wifi_on` only calls the WLAN driver's probe once the core is up (the SDIO function switch is
  `#if 0`, "in soc, wmt turn on wifi directly"). The WLAN driver itself powers the PA: `ahb.c` `HifAhbProbe`
  calls `hwPowerOn(MT6323_POWER_LDO_VCN33_WIFI, VOL_3300)` and `upmu_set_vcn33_on_ctrl_wifi(1)` (`0x418` bit 14, HW
  mode) for as long as Wi-Fi is up, and clears both in `HifAhbRemove`. WMT turns VCN33_WIFI on only for the
  calibration window (as B1 found).
- **The RF path is probably shared with Bluetooth** (INFERRED): CONNSYS has one 2.4 GHz front end. The Y1's
  `WMT_SOC.cfg` sets `coex_wmt_ant_mode=1`, and stock RF calibration turns **both** VCN33_BT and VCN33_WIFI on
  (B1). Bluetooth works through the Y1's antenna today. Whether the board populates the Wi-Fi PA output and
  matching network is unknown and cannot be decided offline: only a scan (W4) shows it.
- **Calibration and address**: the stock userdata held a 514-byte `/nvram/APCFG/APRDEB/WIFI` record. With Wi-Fi
  never enabled at the factory it is presumably the library default (INFERRED; check against the Y1's
  `libcustom_nvram.so` in W1). The Y2 uses address-free `WIFI.defaults` cut from its own library
  (`provision.py`). A Wi-Fi MAC address must be generated locally, as B7 does for Bluetooth.
- **Today's Y1 driver** (`kernel/y1/connectivity/core.c`, `wmt.c`) is Bluetooth only. It already keeps VCN33_WIFI
  for the calibration window. The Wi-Fi function switch (WMT function control for Wi-Fi), the Wi-Fi child device
  and the reference counting the Y2's `core.c` has (`Y2_CONN_WIFI`, `y2_conn_get/put`) are not there.

## 2. Firmware: the blocker

Wi-Fi needs a `WIFI_RAM_CODE` image downloaded into the radio core. The Y1 has none, so for the first time the
firmware cannot come from the device itself.

**The file name is the same on both chips.** MediaTek's MT6572 build compiles the driver with `-DMT6628`
(Makefile line 5), so `kalFirmwareImageMapping` opens `/etc/firmware/WIFI_RAM_CODE_MT6582` (`gl_kal.c`, the
`MT6628` branch; the `request_firmware` variant is `#if 0`). An MT6572 product therefore ships a file named
`WIFI_RAM_CODE_MT6582`. There is no `WIFI_RAM_CODE_MT6572` to look for. Whether its **content** equals the Y2's
cannot be read from kernel source: the blob is a prebuilt from each platform's vendor tree.

Candidates, cheapest and cleanest first:

| | Source | Provenance | Technical risk |
| --- | --- | --- | --- |
| **F1** | The owner's **Y2** stock package (`y2_v3.2.0_FM-20260813`, `system.img:/etc/firmware`): its "several WIFI_RAM_CODE variants" (unnamed in the Y2 audit) | The owner's own MediaTek package | Since the MT6572 name is also `…_MT6582`, the other variants (likely `WIFI_RAM_CODE`, `_E6`, `_MT6628`) are for external chips. Unlikely to help; a listing settles it. |
| **F2** | The owner's Y2 `WIFI_RAM_CODE_MT6582` (160 368 bytes, `4def3752…9b82`) run on the MT6572 | Owner-local, already pinned | Same file name and same driver on both chips, so plausible. **Chosen** (hash comparison below). Remaining risk: the Y1's older Bluetooth patch beside it on the same MCU. A crash is recoverable (CONN power-off; the core's memory is RAM). |
| **F3** | `WIFI_RAM_CODE_MT6582` from an MT6572 product's public stock ROM | A third party's firmware, not from the owner's devices: a new provenance rule; never committed, redistribution not established | Low: built for the MT6572 ROM. Calibration/defaults stay the Y1's own. |

**Hash comparison — 2026-11-07 (owner-approved; files hashed in a scratch folder, nothing run, nothing in the
repo).** Three public vendor trees for the Micromax A102 "Canvas Doodle 3" (an MT6572 phone) carry a full
MediaTek firmware set (GitHub `ifxadi/android_vendor_Micromax_A102`, `LineageOS-13-MT6572/android_vendor_Micromax_A102`,
`adeveloper79/android_vendor_Micromax_A102`, branch `cm-13.0`):

| File | Bytes | SHA-256 | Note |
| --- | --- | --- | --- |
| ifxadi `proprietary/etc/firmware/WIFI_RAM_CODE_MT6582` (git blob `3681b308…c194`) | 160 368 | **`4def37522e2bfd2d4b78e01bf9f567a37e3f67ab3d7b6646ffb317f5b2b49b82`** | **identical to the Y2's** |
| adeveloper79 `etc/firmwere/WIFI_RAM_CODE_MT6572` | 160 368 | (the same git blob `3681b308…c194`) | the same file under an MT6572 name |
| LineageOS-13-MT6572 `etc/firmware/WIFI_RAM_CODE_MT6582` | 160 928 | `ab04b27f…09de7` | a later build |
| ifxadi `WIFI_RAM_CODE_SOC` | 160 464 | `f90f7d4a…46f8` | another build; not loaded by the MT6572 driver |
| ifxadi `mt6572_82_patch_e1_0_hdr.bin` | 36 604 | `0537f046…c696` | `20140411173415a`, `ALPS`, HW/FW `8a00`, info `22000600` (two parts) |
| ifxadi `WMT_SOC.cfg` | 80 | `f4a59b62…2880` | `co_clock_flag=0` (the Y2's: 1) |

All three Wi-Fi images start with the same `MTKW` header layout. **Result: an MT6572 phone's firmware set ships
the Y2's exact `WIFI_RAM_CODE_MT6582`, under the name the MT6572 driver loads. F2 is MediaTek's MT6572 Wi-Fi
firmware of that generation, and the owner's own Y2 copy suffices: no third-party file is needed.**

Limits:
- These are community vendor trees, not a verified stock dump. The board-specific `WMT_SOC.cfg` (no co-clock,
  unlike the Y2) points to the phone's own ROM rather than a copy from an MT6582 device (INFERRED).
- **Correction to §1/F2**: the A102 (MT6572, 2014) uses a two-part `ALPS` Bluetooth patch like the Y2, so the
  Y1's one-part `1319` patch (2013) is an older software release, not evidence of a different radio ROM.
- **New W3 risk**: the Y1 runs its 2013 Bluetooth/ROM patch beside 2014-era Wi-Fi code on the same radio MCU. If
  W3 fails with the Y1's own patch, the patch generation is the next variable. Trying the Y2's own two-part
  `ALPS` patch (owner-local) is then its own admission.

Decision: F2 (the owner's Y2 file, already pinned in `m5-firmware-inventory.json`). F1 and F3 are not needed.

## 3. Design

Extend the Y1 driver rather than fork the Y2's Wi-Fi again:

- **Shared**: the Y2's `wifi/` subtree (about 120k lines, MediaTek GPL fullmac) compiled for the Y1 as a second
  consumer, as `btif.c`/`stp.c`/`hci.c` already are. Its chip check already accepts `0x6572`. The firmware name and pin
  come from the Y1's `conn.h` (as `Y2_CONN_FIRMWARE`), not from `gl_kal.c`'s literal
  `mediatek/mt6582/WIFI_RAM_CODE_MT6582`. The window and IRQ come from DT (`ahb.c` already reads them by name).
  The Y2's objects must stay byte-identical, as in B2 and B5.
- **Y1 core**: `Y1_CONN_WIFI` reference counting beside Bluetooth (the core powers on for either, off when
  neither wants it); no WMT command for Wi-Fi (as MediaTek's SoC path); VCN33_WIFI at 3.3 V with `0x418` bit 14
  (HW mode) held while Wi-Fi runs, as MediaTek's `HifAhbProbe`/`HifAhbRemove`, besides the calibration window;
  the Wi-Fi child platform device with rfkill (`ahb.c` as is, PIO).
- **DT**: `reg` adds `<0x180f0000 0x5c>` "wifi" and `interrupts` adds `<GIC_SPI 123 IRQ_TYPE_LEVEL_LOW>` "wifi"
  (polarity INFERRED from the Y2).
- **Firmware delivery**: the B7 mechanism (`radio-firmware.json`, `/data/firmware/mt6572`), with one more file.
  For F1/F2 it comes from the Y2 package, so the import needs a second source image. That is a platform change and
  is decided after W1.
- **Userspace**: the Y2's Wi-Fi stack (wpa_supplicant, the platform's Wi-Fi readiness and power policy, Reborn's
  Wi-Fi screens) as on the Y2. `CONFIG_CFG80211` and the network options in the Y1 kernel fragment.

## 4. Stages

| Stage | What | Writes to the Y1 |
| --- | --- | --- |
| **W0** | This plan and the roadmap audit | none |
| **W1** | Firmware source decided (§2), with the hash comparison if admitted. Offline: its inventory entry (size, SHA-256, header/version strings), the Y1's `WIFI` NVRAM default against `libcustom_nvram.so`. The driver check is done (§1). | none |
| **W2** | The driver extended (built, not loaded): the shared `wifi/` for the Y1, the Y1 core's Wi-Fi function, the DT node unchanged (no `wifi` resources, so nothing probes); host tests; the Y2's objects byte-identical | none |
| **W3** | One attempt from userspace: CONN on (as B5), the WMT Wi-Fi function, VCN33_WIFI, the firmware download, the HIF chip ID (`MCR_WCIR`), the firmware's ready event; **no scan, no transmission**; then off | firmware into the radio core |
| **W4** | `wlan0`, a **passive** scan first (receive only), then an active scan: shows whether the Wi-Fi RF path works on this board | radio receive, then transmit |
| **W5** | Association with the owner's access point, DHCP, DNS, a bounded transfer; coexistence with Bluetooth audio; RAM and CPU cost | radio on |
| **W6** | Production: the firmware import, the Y2's Wi-Fi platform service and Reborn's screens on the Y1 | image |

## 5. Admission

| Stage | Item | Bound |
| --- | --- | --- |
| W1 | **Firmware source** | F1, F2 or F3 (§2): the owner decides; owner-local only, never committed |
| W1–W2 | **Prepare** | documents, host code and tests; nothing on the Y1 |
| W3 | **Wi-Fi firmware boot** | the chosen file pinned by SHA-256; no RF emission |
| W4 | **Scan** | passive, then active, in the owner's regulatory domain |
| W5 | **Wi-Fi on** | the owner's own network only |
| W6 | **Production** | its own build and regression check |

## 6. Risks

- The board may lack the Wi-Fi RF output (Innioasis switched Wi-Fi off, perhaps for that reason). W3 can pass
  and W4 still find nothing. Then the work stops at W4 with the driver kept off.
- F2 may crash the radio core beside the Y1's 2013 Bluetooth patch (§2). Recovery is CONN power-off. It may also disturb Bluetooth on the
  same core until the next power cycle.
- No factory Wi-Fi calibration: TX power and frequency offset come from defaults. Range and throughput may be
  worse than the Y2's.
- Memory: the fullmac driver plus wpa_supplicant on 512 MiB. The Y2 runs the same stack. Measure in W5.
- The driver is the same; the firmware may not be (§2). With F2, failures at W3 point first at the firmware.
- Effort (INFERRED): W2 is mostly the Y1 core's Wi-Fi function (reference counting, the PA rail) and the
  shared-source plumbing; W3/W4 decide whether there is more to do.

## 7. W1 — 2026-11-07

Owner-approved (W1–W2). Offline, read only; nothing on the Y1.

- **Firmware**: `docs/knowledge/evidence/y1-firmware-inventory.json` lists `WIFI_RAM_CODE_MT6582` (160 368 bytes,
  `4def3752…9b82`). It comes from the owner's Y2 package, cross-checked against the A102 set (§2), and is
  installed at `/data/firmware/mt6572/`. It is marked `optional`: Bluetooth-only staging and the B7 import (the
  Y1's own `system.img`) do not need it.
- **The Wi-Fi record**: the Y1's stock `/system/lib/libcustom_nvram.so` (37 508 bytes, `0000ccf1…3ebd`) holds
  exactly one `WIFI_CFG_PARAM_STRUCT`, at file offset `0x3cf4` (VA `0x4cf4`, the second `PT_LOAD`): version
  `0x104`, `ucTxPwrValid` 1, TX power bytes `26 26 00 00 20 20…`, MAC all zero, SHA-256 `9cfed6e4…a8b4`. It differs
  from the Y2's default (`eb5e8722…`). The owner's stock userdata `/nvram/APCFG/APRDEB/WIFI` (514 bytes, read
  with `debugfs` from the backup at the `USRDATA` offset) is that record byte for byte plus a 2-byte checksum. It
  was written once at the factory (02:00) and never again, while `BT_Addr` was written at 03:28. **This Y1 has
  no factory Wi-Fi calibration and no Wi-Fi MAC**, so its record is the default with a locally generated
  address, as B7 does for Bluetooth.
- **Provisioning** (`tools/connectivity/provision.py --profile y1`): an inventory entry marked `optional` is
  staged only when present (and checked when present). `--stock-custom-nvram` now takes the Y1's own library
  (size, SHA-256, ELF header, record layout and the record's hash pinned) and stages `WIFI.defaults`; a Y2
  library is refused. The receipt names the Y1 library. Profile `y2` unchanged.
- **Tests**: `tests/test_y1_provision.py` 7 (3 new: optional staging with/without/wrong, the default cut from a
  synthetic library and refused when changed or with an address, the pins). The real library gives the pinned
  record.

## 8. W2 — 2026-11-07

The Y1 driver extended with Wi-Fi, **built, not loaded**: no device tree has the `wifi` window or interrupt, so
no Wi-Fi device is created. Nothing was built for or sent to the Y1.

- **The Y2's fullmac, in place**: `run.py` mounts `kernel/platform` at `drivers/y2` in every build, the Y1's
  too, so the 128 Wi-Fi files are not copied. Overlay `0040-innioasis-y1-wifi-drivers-makefile.patch` adds one
  line to `drivers/Makefile`: `obj-$(CONFIG_BT_INNIOASIS_Y1_WIFI) += y2/connectivity/wifi/`. Overlay 0036 adds
  `BT_INNIOASIS_Y1_WIFI` (bool, `depends on BT_INNIOASIS_Y1 && CFG80211=y && RFKILL=y`).
- **Three Y2 edits, output unchanged**: `wifi/hif.h` includes `"conn.h"` instead of `"../conn.h"`; `wifi/Makefile`
  adds `-I$(src)/..` and, only with the Y1 option, `-I$(srctree)/drivers/bluetooth` first (so the Y1 build finds
  the Y1's header); `gl_kal.c` requests `Y2_CONN_FW "WIFI_RAM_CODE_MT6582"`, the same string on the Y2.
  **Evidence the Y2 is unchanged**: production kernel (`production.config`) at HEAD (`out/w2-y2-before`) and
  after (`out/w2-y2-after`): `Image` `a68fdffd…aac21`, `vmlinux` `b5ae438d…eb202` (B2's values), and all 48
  connectivity and Wi-Fi objects byte-identical.
- **Y1 header and policy** (`kernel/y1/connectivity/conn.h`, `y1-conn-policy.h`): `Y2_CONN_WIFI`, the fields the
  fullmac reads (`wifi_wanted`, `wifi_factory`, the child device), the Wi-Fi firmware in the pinned table
  (`mt6572/WIFI_RAM_CODE_MT6582`, the owner's Y2 hash); MediaTek's window `0x180f0000`/`0x5c` and SPI 123; the
  Wi-Fi record rule (§7: the record with bytes 4–9 zeroed must hash to `9cfed6e4…`, the MAC unicast, non-zero
  and not the Bluetooth address); `Y1_CONN_WIFI_WMT_FUNCTION 0`.
- **Y1 core** (`kernel/y1/connectivity/core.c`): `y2_conn_get`/`put` take Wi-Fi beside Bluetooth with its own PA
  rail (VCN33_WIFI, `0x418` bit 14 hardware mode: MediaTek's `HifAhbProbe`), held while Wi-Fi runs; no WMT
  function command for Wi-Fi (MediaTek's MT6572 `wmt_func_wifi_on`; the Y2 sends `01 06 02 00 03 01`, a W3
  variable). A root-only `wifi_factory` attribute (512 bytes, after the Bluetooth record, before activation).
  Activation creates the Y2's `y2-mt6582-wifi` child (rfkill `y2-wifi`) only with the Wi-Fi option, the record
  and the `wifi` window; without them it registers Bluetooth only, as before. The window and interrupt are
  optional in the node but exactly MediaTek's when present. Recovery releases and rebinds the Wi-Fi driver, and
  shutdown unregisters it (both as on the Y2). The bring-up modes (`chip-id`, `wmt`) are unchanged.
- **Builds** (W2 fragment `out/w2-y1.config`: the thirty-fourth fragment with `WIRELESS`, `CFG80211`, the signed
  regulatory database options and `BT_INNIOASIS_Y1_WIFI`; `CONFIG_WLAN` is only the vendor-driver menu and needs
  `NETDEVICES`, so it is not used; build script `out/w2-y1-build.sh`): `out/w2-y1-wifi-3`, zImage
  `fce5f47c…44ef5`, `.config` `01389d97…2f92`, `y1-init` `d278e15b…` (B7's). The fullmac is compiled against
  `drivers/bluetooth/conn.h` and requests `mt6572/WIFI_RAM_CODE_MT6582`. Its 47 warnings (missing prototypes,
  fall-throughs in MediaTek's code) are the Y2 production build's own, none new and none outside `wifi/`. The
  thirty-fourth variant (Bluetooth only, `y1-boot.sh thirty-fourth`, `out/w2-y1-34`): no warning; `.config`
  `ddd1ff8b…` (build 5's, so the overlays change nothing while Wi-Fi is off), DTB and `y1-init` identical,
  zImage-dtb `d051cc20…d330` (build 5: `b845699f…`; `core.c` changed), BOOTIMG `7e988938…35c9`. Not for flashing.
- **Tests** (sandbox): `test_y1_connectivity` 29 OK. New: the Wi-Fi window/IRQ in the wiring table; the record
  rule (a good record; zero, multicast, the Bluetooth address, another TX power, a short record refused); the pin
  is the inventory's record; the driver's actual `function_rail`/`function_switch`/`y2_conn_rail` on the register
  model (Wi-Fi: VCN33_WIFI with its hardware bit, no WMT command; Bluetooth still sends its command; off clears
  both); 0040 adds exactly its one line and 0036 the option; the B7 import set excludes the optional Wi-Fi file.
  The B6 open/close test now runs the real helpers too. `test_y1_provision` 7, `test_wifi` 1, `test_overlays` OK;
  `test_overlay_hashes` OK on the host (in the sandbox the Y1 set fails, as before). `test_connectivity`: the
  regulatory download error and `test_e2_feature_page_quirk_with_actual_kernel_consumer`
  (`Y2_CONN_EXT_FEATURES_QUIRK_CHIP` undeclared) both fail the same at HEAD without these changes: pre-existing
  (B6), not fixed here. The code-quality ratchet passes.
- **For W3** (own admission, roadmap audit first): a variant with the `wifi` window and interrupt in the node
  and the Wi-Fi fragment; the owner's `WIFI_RAM_CODE_MT6582` and the Y1 `WIFI.defaults` with a generated MAC in
  `/data/firmware/mt6572` and `/data/connectivity`; S41 writes `wifi_factory` before `activate`; `rfkill unblock
  wifi` then runs the fullmac's probe: firmware download, `MCR_WCIR` chip ID, the firmware's ready event and
  `wlan0` up with **no scan** (W4). Open: whether the firmware boots beside the 2013 Bluetooth patch, whether it
  needs the WMT Wi-Fi function command (the Y2's), and the regulatory database on the Y1 rootfs.

## 9. W3 build — 2026-11-07

Owner-approved; roadmap audit first ("Y1 Wi-Fi W3, first Wi-Fi firmware boot"). One manual firmware boot:
`wlan0` registered, then off. **No scan, no association, no transmission.**

- **Variant thirty-fifth**: `kernel/config/y1-thirty-fifth-boot.config` (the thirty-fourth fragment, `WIRELESS`,
  `CFG80211` with the signed regulatory database as the Y2, `BT_INNIOASIS_Y1_WIFI`, LOCALVERSION
  `-y1-thirty-fifth-boot`; the thirty-fourth initramfs list), `kernel/dts/innioasis-y1-thirty-fifth-boot.dts` (the
  thirty-fourth tree plus `<0x180f0000 0x5c>` "wifi" and `<GIC_SPI 123 IRQ_TYPE_LEVEL_LOW>` "wifi" in the
  connectivity node, nothing else), `y1-boot.sh` and `y1_boot.py` (the node's two new lines checked in the
  compiled tree). Build `out/y1-thirty-fifth-boot-1`: BOOTIMG
  **`fc6cf8d4b0d8e663cd67dd8eb99f1b993531982a827ae97fbc5dc9e41e996ae2`** (4 157 440 bytes; build 5: 3 788 800),
  zImage-dtb `96afec3f…a2ec`, DTB `6ba7ac9a…6316`, `.config` `6b48891c…7494`, `y1-init` `d278e15b…` (unchanged). The
  47 warnings are the fullmac's own (§8).
- **S41** (shared with the thirty-fourth, inert there): creates `/data/network/enabled` as `0` if absent (the Y2's
  `S02y2-data` does this; the Y1 does not run it), so `y2-radiod` blocks every Wi-Fi switch at its start;
  `wifi_compose` (once: `WIFI.defaults` bytes 0–3 and 10–511 around a random unicast, locally administered MAC);
  `wifi_record` (root 0600 512 bytes to `wifi_factory` after the Bluetooth record, before `activate`; refused or
  absent means Bluetooth only); `stop-wifi` as the Y2 (`y2-radio wifi off-runtime`).
- **Why nothing transmits by itself**: on the Y1 no service starts `wpa_supplicant` (the Y2's
  connectivity daemon is not in `y1-services`); the root filesystem's `/etc/network/interfaces` has only `lo`;
  no hotplug or cron rule names `wlan0`; `y2-radiod` blocks Wi-Fi at its start and unblocks it only on a Reborn or
  `y2-radio` request (do not open Reborn's Wi-Fi screen during W3). The driver's rfkill switch starts blocked.
  Registering the interface leaves it down; nothing here sets it up.
- **Firmware folder**: a later Bluetooth import (only while its firmware is missing) replaces the whole
  `/data/firmware/mt6572` and would drop the two Wi-Fi files; copy them again then. `firmware status` ignores them.
- **Tests** (sandbox): `test_y1_connectivity` 33 (4 new: `wifi_compose` from the real S41, nothing without a Wi-Fi
  build or defaults and an existing record kept, the tree adds only the two node lines, the fragment only the
  Wi-Fi lines); `test_y1_provision` 7, `test_y1_bt_record`, `test_wifi`, `test_overlays` OK; the ratchet passes.

**Procedure** (owner; the Y1 on build 5 first; `Y1SSH='ssh -i out/y1-ssh/id_ed25519 -o
UserKnownHostsFile=out/y1-ssh/known_hosts root@10.42.0.1'`):

1. On the PC, stage the files with the provisioning tool. You need the Y1 3.0.2 `system.img` (raw ext4,
   from the public `rom.zip`, as B7) and the Y2 package's `system.img` (`simg2img` first if it is sparse):

       mkdir out/w3-in
       debugfs -R "dump /etc/firmware/WMT_SOC.cfg out/w3-in/WMT_SOC.cfg" <Y1 system.img>
       debugfs -R "dump /etc/firmware/mt6572_82_patch_e1_0_hdr.bin out/w3-in/mt6572_82_patch_e1_0_hdr.bin" <Y1 system.img>
       debugfs -R "dump /etc/firmware/modem_1_2g_n.img out/w3-in/modem_1_2g_n.img" <Y1 system.img>
       debugfs -R "dump /lib/libcustom_nvram.so out/w3-libcustom_nvram.so" <Y1 system.img>
       debugfs -R "dump /etc/firmware/WIFI_RAM_CODE_MT6582 out/w3-in/WIFI_RAM_CODE_MT6582" <Y2 system.img>
       python3 tools/connectivity/provision.py --profile y1 --owner-firmware out/w3-in \
           --stock-custom-nvram out/w3-libcustom_nvram.so --output out/w3-firmware

   It verifies every file against the inventory, the Y1 library and the record hash.
2. Copy the two Wi-Fi files and check them:

       scp -i out/y1-ssh/id_ed25519 -o UserKnownHostsFile=out/y1-ssh/known_hosts \
           out/w3-firmware/WIFI_RAM_CODE_MT6582 out/w3-firmware/WIFI.defaults root@10.42.0.1:/data/firmware/mt6572/
       $Y1SSH 'cd /data/firmware/mt6572 && chmod 600 WIFI_RAM_CODE_MT6582 WIFI.defaults && sha256sum WIFI_RAM_CODE_MT6582 WIFI.defaults && sync'

   Expected: `4def3752…9b82` and `9cfed6e4…a8b4`.
3. Flash BOOTIMG `fc6cf8d4…6ae2` as for build 5 (keep build 5, `01f78b58…`, for the way back) and boot.
4. Read-only checks; Bluetooth must work as before:

       $Y1SSH 'dmesg | grep -i -E "18070000|y2-mt6582-wifi|cfg80211|regulatory"; cat /sys/devices/platform/18070000.connectivity/status;
               rfkill list; cat /data/network/enabled; ls -ln /data/connectivity/wifi-record; pgrep -l wpa_supplicant; ls /sys/class/net'

   Expected: `MT6572 connectivity owner ready (Bluetooth, Wi-Fi)`; an rfkill entry `y2-wifi` (Wireless LAN),
   **soft blocked: yes**; `0`; `-rw------- 1 0 0 512 …`; no `wpa_supplicant`; no `wlan0`.
5. The firmware boot (keep a second session with `dmesg -w`). `<n>` is the `y2-wifi` ID from `rfkill list`:

       $Y1SSH 'rfkill unblock <n>; sleep 15; dmesg | tail -60; cat /sys/devices/platform/18070000.connectivity/status; ip link show wlan0; iw dev'

   Expected: if Bluetooth was off, the core's power-on (`CONN chip=6572`, the WMT commands, `WMT RF calibration`);
   then `y2-mt6582-wifi y2-mt6582-wifi: Wi-Fi interface wlan0 registered`; `functions=` with bit 3 (`0x8`, or
   `0x9` with Bluetooth on); `wlan0` **state DOWN**; `iw dev` shows `phy#0`/`wlan0` (its address expected to be
   the one in `wifi-record`, INFERRED: the fullmac reads it from the record). **Do not** run `ip link set wlan0 up`, `iw … scan` or `wpa_supplicant` (W4).
   A failure: `Wi-Fi enable failed: <error>` (the switch blocks itself again), `firmware request failed`, or
   `connectivity failure … ` (then the core recovers; Bluetooth comes back with it).
6. Off: `$Y1SSH 'rfkill block <n>; sleep 3; ls /sys/class/net; cat /sys/devices/platform/18070000.connectivity/status'`
   (no `wlan0`; bit 3 cleared). Repeat 5–6 once more.
7. Abort at any time: `rfkill block <n>`; if that hangs, power off by the 3 s centre hold and boot again; the way
   back is build 5.

Record every line in `evidence-private/2026-11-07-y1-wifi-w3/`: the whole `dmesg` from the unblock, the `status`
lines, `iw dev`, any WMT or Wi-Fi error, and whether Bluetooth audio still plays afterwards.

## 10. W3 run — PASS — 2026-11-07

`evidence-private/2026-11-07-y1-wifi-w3/` (pre-unblock dmesg, `run1-unblock.txt`, `run1-phy.txt`,
`run1-cycles.txt`, `run1-bluetooth.txt`). The Y1 on the thirty-fifth boot (BOOTIMG `fc6cf8d4…`).

- **Firmware source**: `WIFI_RAM_CODE_MT6582` read out of `system.img` of the y1-community release `3.2.0-fm`
  (`rom_y2`, the build `INSTALL.md` names as Innioasis's v3.2.0; SHA-256 `5ce2c81d…7b60` as published), the
  pinned `4def3752…9b82`. The Y1 inputs from the owner's backup. `provision.py --profile y1` verified all five
  files and cut `WIFI.defaults` (`9cfed6e4…`). The procedure's step 1 lacked the modem image, which the Y1
  inventory requires; added.
- **Before**: both Wi-Fi files in `/data/firmware/mt6572` (root 0600; `firmware status` still `Ready`); after a
  reboot S41 composed `wifi-record` (with the MAC zeroed it hashes to the default; the MAC unicast and locally
  administered) and the driver accepted it: `owner ready (Bluetooth, Wi-Fi)`, the `y2-mt6582-wifi` device, rfkill
  `y2-wifi` soft blocked; `/data/network/enabled` 0, no `wpa_supplicant`, no `wlan0`.
- **Unblock** (`rfkill unblock 1`, Bluetooth on): `<wifi> send Wi-Fi Start command`, 66 ms later `Wi-Fi interface
  wlan0 registered`; `functions=0x9`, `error=0`, no recovery; rfkill `phy0` added; `wlan0` **DOWN**; its address is
  the one in `wifi-record` (compared on the device). `iw phy`: managed mode, 2.4 GHz channels 1–14 at 20 dBm,
  12–14 no-IR; `iw reg get`: `country 00` (the world domain: `regulatory.db` was not loaded, see below).
- **The open W3 questions, answered**: the 2014-era Wi-Fi firmware (the Y2's, from the MT6572 phone set) runs
  beside the Y1's 2013 Bluetooth patch; it needs **no WMT Wi-Fi function command** (`Y1_CONN_WIFI_WMT_FUNCTION 0`,
  as MediaTek's MT6572 driver); the HIF window `0x180f0000` and SPI 123 are right.
- **Block / repeat**: block removes `wlan0`, `functions=0x1`; a second unblock boots the firmware again (73 ms to
  `wlan0`), a second block again clean. Bluetooth powered throughout (BlueZ `Powered: yes`), `error=0`,
  `recoveries=0`, no WMT or Wi-Fi error in the whole log. **No scan, no association, `wlan0` never up.**
- **For W4**: `cfg80211` (built in) tries `regulatory.db` at 1.2 s, before the root file system holds it
  (`/lib/firmware/regulatory.db` and `.p7s` are there), so the domain stays the world default; before the scan,
  `iw reg reload` (or the database in `/data/firmware`) and the owner's country. W4: passive scan first.

## 11. W4 build — 2026-11-07

Owner-approved, country AT; roadmap audit first ("Y1 Wi-Fi W4, the first scans").

- **The fullmac has no passive station scan**: `ais_fsm.c` sets `SCAN_TYPE_ACTIVE_SCAN` for every AIS scan
  (passive only with `CFG_SUPPORT_RDD_TEST_MODE`), and `mtk_cfg80211_scan` turns a request without SSIDs into a
  wildcard probe. `iw … scan passive` would therefore transmit. Added, for the Y1 only (`-DY2_WIFI_PASSIVE_SCAN`
  from the Wi-Fi Makefile with `BT_INNIOASIS_Y1_WIFI`): cfg80211's rule, a request without SSIDs listens only.
  `gl_cfg80211.c` records it in the Y2 glue's `GL_HIF_INFO_T` (`hif.h`), and `ais_fsm.c` applies it to the user's
  own scan (`AIS_STATE_SCAN`/`ONLINE_SCAN`, never a join's) and clears it; `wpa_supplicant` always sends the
  wildcard SSID, so its scans stay active. The firmware already takes the type (`rCmdScanReq.ucScanType`) and
  chooses the dwell time.
- **The Y2 unchanged, by line numbers**: a first version inserted lines into `wlan_oid.c`, `ais_fsm.c` and
  `gl_cfg80211.c` under `#ifdef`; the Y2's `vmlinux` changed (`4bb3ddaf…`) because the vendor's assert and log
  macros carry `__LINE__`. The hooks now end two existing vendor lines (each file keeps its line count) and the
  macros live in `hif.h`. Y2 production build `out/w4-y2-after`: `Image` `a68fdffd…aac21`, `vmlinux`
  `b5ae438d…eb202`, all 48 objects identical to `out/w2-y2-before`.
- **Build** `out/y1-thirty-fifth-boot-2`: BOOTIMG **`5a78d9d4b74153e59bbef6cc856902eb44152fda4d78e421f82699e27397eb75`**,
  zImage-dtb `25fcf381…c327`; DTB, `.config` and `y1-init` as build 1; the fullmac's 47 known warnings.
- **Regulatory database** (on build 1, the radio blocked): `iw reg reload` loads the root file system's signed
  `regulatory.db`; `iw reg set AT` gives `country AT: DFS-ETSI`, 2400–2483 MHz at 20 dBm. Not kept across a
  reboot: set again before each scan session (W6: at boot).
- **Tests**: `Y1WifiW4` (the flag only with the Y1 option, the hooks end their two vendor lines at their old line
  numbers, `wlan_oid.h` untouched).

## 12. W4 run — PASS — 2026-11-07

`evidence-private/2026-11-07-y1-wifi-w4/` (`passive-raw.txt`, `active-raw.txt`: neighbouring networks' names
and addresses, private). The flashed `BOOTIMG` read back from the Y1 (physical `0x2b80000`, 4 157 440 bytes) is
build 2, `5a78d9d4…eb75`. Domain set first: `iw reg reload`, `iw reg set AT` (`country AT: DFS-ETSI`).

- **Passive** (`rfkill unblock 1`, `ip link set wlan0 up`, `iw dev wlan0 scan passive`, then down and blocked):
  **3 networks** (channels 1, 1, 11; −34 to −88 dBm), 1.36 s for the band (about 100 ms per channel: beacon
  listening), **no probe responses** in the result. **The board's Wi-Fi receive path reaches an antenna.**
- **Active** (`iw dev wlan0 scan`, same wrap-around): **3 networks** (channels 1, 6, 11; −33 to −96 dBm) in
  0.37 s, **all from probe responses**: the Y1's probe requests went out and were answered. **The Wi-Fi transmit
  path works.** Two networks were seen by both scans, one only by each (the weakest ones). The shorter time also
  shows the passive flag reaches the firmware.
- **Each time**: firmware start and `wlan0` within 70 ms of the unblock; afterwards `functions=0x1`, `error=0`,
  no recovery, no `wlan0`, the switch blocked; Bluetooth `Powered: yes`; no Wi-Fi, WMT or core error in the log.
  No association, no `wpa_supplicant`.
- **Open for W5**: association with the owner's network (WPA2, `wpa_supplicant` as on the Y2), DHCP, DNS, a bounded
  transfer, coexistence with Bluetooth audio, RAM/CPU; the TX power and frequency offset are uncalibrated defaults
  (signal levels equal to the passive scan's, so no obvious loss); the domain at boot (W6).

## 13. W5 — association with the owner's network — 2026-11-07

Owner-approved (`BotanischerGarten`, WPA2, country AT); roadmap audit first. `evidence-private/2026-11-07-y1-wifi-w5/`.

- **Credentials**: the owner ran one command in his own terminal (passphrase read without echo, `wpa_passphrase`,
  its comment lines, including the plaintext one, removed): `/data/network/wpa_supplicant.conf`, root 0600, holds
  `ctrl_interface=/run/wpa_supplicant`, `update_config=0`, `country=AT` and one network block with a 64-hex PSK.
- **Build 2 failed to start `wpa_supplicant`** (`nl80211 … Failed to initialize driver interface`, nothing
  associated): the Y1 kernel lacked `CONFIG_PACKET` (`AF_PACKET`, which `wpa_supplicant`'s EAPOL and `udhcpc` need;
  the Y2 has it). The comparison of the Y2's networking options with the Y1's left only this one. **Build 3**
  (`out/y1-thirty-fifth-boot-3`, BOOTIMG **`2b881029f8154b4e42d32f7b90d73c4db40bd059a389421ba1a34bc23b5ded86`**, read
  back from the Y1; `.config` `5b539021…72aa`; DTB, `y1-init` unchanged; the fragment adds `CONFIG_PACKET=y`).
- **Run on build 3** (by hand with the Y2's tools: `wpa_supplicant -D nl80211`, BusyBox `udhcpc` with a script in
  `/run/y1-w5`; not yet the Y2's connectivity daemon, W6):
  - `COMPLETED` 6.3 s after `wpa_supplicant` started: WPA2-PSK, CCMP/CCMP, 2412 MHz, RSSI −33 dBm, link 135 Mbit/s.
  - DHCP at once: `192.168.1.37/24`, gateway and DNS `192.168.1.1`, default route on `wlan0`.
  - Gateway ping 5/5 (1.2–44 ms); DNS through the router; `github.com` by name 3/3 (25–59 ms): **the Internet
    works**.
  - **Five 8 MiB HTTP downloads from the owner's PC, every one with the right SHA-256** (`35d3078c…`); RAM:
    `wpa_supplicant` 5.6 MB, `udhcpc` 0.9 MB RSS, MemAvailable 434 MB of 496; CPU 3–5 % busy.
  - **Throughput low and erratic**: 2.1, 1.1, 1.2, 1.3 and 8.3 Mbit/s (the Y2: about 45). The PC was on the
    router's 5 GHz network (1170 Mbit/s), not the limit; Bluetooth off (`off-runtime`) did not help (1.2, 1.3),
    Bluetooth on again gave the 8.3. `wlan0`: 12 151 packets received, 250 dropped, no TX error; the reported
    TX rate fell to 5.5–11 Mbit/s at times with a bogus −10 dBm, then back to 135 / −34 dBm. **The network has two
    2.4 GHz access points and the Y1 roamed between them during the test** (`…:ec` → `…:56` → `…:ec`), which stalls
    a transfer. Not settled: how much is roaming, how much the uncalibrated TX/frequency defaults or the RX drops.
  - Teardown: `udhcpc` and `wpa_supplicant` stopped, address and route removed, `wlan0` down, Wi-Fi blocked;
    Bluetooth powered, no Wi-Fi, WMT or core error at any time.
- **Result**: association, DHCP, DNS, the Internet and intact transfers PASS; **performance open**.
- **Next (W5b)**: the transfers again pinned to one access point (`bssid=` in a test copy of the
  config) and longer; an upload; the RX drop counter during a transfer; Bluetooth audio with the owner's headset at
  the same time. Then W6: the Y2's connectivity daemon and Reborn's Wi-Fi screens, the regulatory domain at boot.

## 14. W6 — Wi-Fi as a product — 2026-11-07

Owner-approved ("continue with W6, make it a product"; W5b, the throughput check, stays open); roadmap audit first
("Y1 Wi-Fi W6"). Goal: a Y1 gets Wi-Fi the way a Y2 does. The owner copies the firmware packages over USB, switches
Wi-Fi on in Reborn and picks a network, with nothing done by hand. Read on the Y1 first: no `/dev/y2-calibration`,
no BusyBox `inotifyd`, the Wi-Fi child device under the connectivity node, the root still beta 9.3.12.

**What the Y2 does that the Y1 lacked** (traced in the platform):

| Y2 | Y1 before W6 | W6 |
| --- | --- | --- |
| `usr/libexec/y2/connectivity`: factory, calibration, activation, `wpa_supplicant -g`, radiod, `connectivity-daemon` | radiod only | S41 runs the same without factory and calibration |
| `connectivity-daemon` supervises `y2-calibration` (MD1, MT6582) | — | the image's `radio-firmware.json` says `"calibration": false`; the monitor skips it |
| `service-daemon`: `network.json` (Reborn's Wi-Fi state, stale after 15 s), the DNS probe (`Online`), the Wi-Fi self-heal and power policy, coexistence, ntpd | not run (it would also start the Y2's USB, card, update and report work) | `/etc/y2linux/service-policy.json` restricts it to that network part |
| `S02y2-data`: `wpa_supplicant.conf` (`country=00`, `update_config=1`), `/run/wpa_supplicant` | — | S41's `layout` |
| `firmware scan` installs the Wi-Fi firmware from the Y2 package | B7: the Y1's `system.img` (Bluetooth only); the Wi-Fi files copied by hand | the Y1's set gains the Wi-Fi default and the Y2 package's file as a supplement |

- **Platform** (reaches the Y1 with a new Y2ROOT; the Y2's behaviour unchanged: no file, no field):
  - `radiofw`: `radio-firmware.json` may name further `libraries` of the same image (a default's `library`),
    a `supplement` (files of another package's `/etc/firmware`, installed beside the set and not needed for
    `Ready`) and `calibration`. A copy that is not the set's image but holds the supplement is installed,
    deleted and the radios restarted, in either order. The set's install keeps an installed supplement. The
    configured folder and its note stay while the supplement is missing. `firmware install` takes either image.
  - The connectivity monitor starts `y2-calibration` only when the image's set says `calibration` (default true).
  - `service-daemon`: with `/etc/y2linux/service-policy.json` (`{"schema":1,"network_only":true}`; any other
    content except an explicit `false` restricts too) each pass keeps `network.json` with the DNS probe and the
    self-heal, `coexistence.json`, the Wi-Fi power policy, `time-status.json`, ntpd once `Online` and the hourly
    time floor. It runs no USB, media, firmware, update, report or health work, and has the dark cadence from the
    start.
- **Y1 set** (`initramfs/y1-bluetooth/etc/y2linux/radio-firmware.json`): `WIFI.defaults` from the Y1's own
  `/lib/libcustom_nvram.so` (`0x3cf4`, 512 bytes, `9cfed6e4…`, as `provision.py`); supplement
  `WIFI_RAM_CODE_MT6582` (`4def3752…`); `calibration: false`; the note names both packages. **Checked with the
  real images** (the owner's stock system partition `ANDROID-from-backup.img` and the Y2 3.2.0 `system.img`):
  `firmware install` in both orders and the owner's flow (`firmware scan` with each copy in `Music/Firmware`)
  install all five files with the pinned hashes and delete both copies.
- **Kernel** (`core.c`): `wifi_factory` is also taken once after activation, while there is no Wi-Fi device. It
  registers the device then (`Wi-Fi registered after activation`). Activation and this late path hold
  `recovery_lock`, so neither races a recovery. Before, a Wi-Fi import while running needed a restart.
- **S41** (`S41y2-connectivity`): the Y2's `/data/network` defaults and `/run/wpa_supplicant`; the firmware scan
  also while a copy waits in the folder. After activation it writes the late Wi-Fi record. With the Wi-Fi device
  (none on a Bluetooth-only Y1: no supplicant RAM): `iw reg reload` (cfg80211 read `regulatory.db` before the
  root existed, so the owner's country applies), `wpa_supplicant -g`, radiod, `connectivity-daemon`, and the Y2's
  `readiness-supervise` for the service daemon, the last only when `/usr/sbin/y2-platform` knows the policy
  (`grep -q service-policy.json`: beta 9.3.12 does not and would run the Y2's whole service). `stop-daemon` also
  stops these.
- **S45y1-mtp**: when a copy finishes (its receipt's refresh generation) and `Music/Firmware/system.img` exists, it
  runs `S41 firmware` (BusyBox has no `inotifyd`; the receipt loop already runs every second).
- **Tests**: platform: `radiofw` 17 (3 new: a supplement optional and named once, the calibration switch, both
  orders through `scan` from synthetic ext4 images), the supervisor without calibration, `tests/service.rs` the
  network-only pass (no platform child, no USB record, `Online`; an unreadable policy restricts). Y1:
  `test_y1_connectivity` 41 (W6: the late record, the scan conditions, the MTP hook, the service policy and its
  guard, the driver's locking; B7's set test now checks the Wi-Fi default and the supplement against the
  inventory), with `test_y1_provision`, `test_y1_bt_record` and `test_wifi`: 50 OK, 1 skipped. Gate: fmt, clippy,
  armv7, quality ratchet and unit tests pass. The goldens fail the same 21 cases as `HEAD` (media status, report
  upload, reset plan; measured with and without these changes). One gate run also failed
  `service-daemon-wifi-online` and `service-daemon-usb-internal-storage`. They did not fail again in three full
  runs or five runs alone. Record compatibility and the Reborn contract pass.
- **Build** `out/y1-thirty-fifth-boot-5`: BOOTIMG **`eeede6ed2bb7387ab1a5fca1c2bb0a5ef9b955e58bdeefe7ee008f8658aa5ac5`**
  (4 177 920 bytes), zImage-dtb `54e85ead…ca87`, `y1-init` `d8f41d4e…e5` (the policy override), DTB and `.config`
  as build 3; the fullmac's 47 warnings, none elsewhere. (Build 4, `f49d4182…`, lacked the policy; not for
  flashing.)

**Live run on build 3 — PASS** (`evidence-private/2026-11-07-y1-wifi-w6/`; the new S41 copied into the RAM overlay,
beta 9.3.12's platform; the owner's `wpa_supplicant.conf` changed from `update_config=0` to `1` so Reborn can save
networks):

- S41 restart: `wpa_supplicant` on `/run/wpa_supplicant/global`, radiod, the monitor; `country AT: DFS-ETSI` after
  the reload. Bluetooth powered throughout.
- `y2-radio wifi on` (Reborn's switch): wlan0, scan, `COMPLETED` about 12 s later through the monitor's
  `interface_add`, DHCP by the Y2's action hook, default route and resolver. Gateway ping, DNS by name and HTTP
  work. Off: `wlan0` gone, the radio blocked, the supplicant interface removed. On again: an address after 8 s.
- The power daemon's order (`stop-wifi`, `stop-bluetooth`, `stop-daemon`): each under 0.1 s, `wlan0` gone. Start
  again: Wi-Fi back as the owner left it (an address after 16 s).
- Cost connected: `wpa_supplicant` 6.2 MB, monitor 3.6 MB, radiod 3.1 MB RSS; CPU idle; MemAvailable 433 MB.
- As expected with beta 9.3.12's platform: `status wifi` stays `Authenticated / dns_unavailable`, because nothing
  runs the DNS probe and no `network.json` is published (the service daemon waits for a root that knows the
  policy). The monitor restarts `y2-calibration` every 30 s; the platform change removes this.
- Found: after an off/on cycle `/run/y2/wifi-action.pid` was gone while the action hook's `wpa_cli` ran. The old
  instance outlived its pid file, so a later stop by pid file misses it. This is in the Y2's shared hook code: open,
  not changed here.
- Afterwards: Wi-Fi off again (`/data/network/enabled` 0, as before W6).

**Owner procedure (build 5)**: flash BOOTIMG `eeede6ed…` as for the earlier builds (keep build 3, `2b881029…`, for
the way back), boot. Expected: Bluetooth as before; with Wi-Fi switched on in Reborn the owner's network as in the
live run (still `dns_unavailable` until the new root). With the next Y2ROOT on the Y1: `Online` in Reborn,
`network.json`, no `y2-calibration`, and the import from `Music/Firmware` (Y1 package first, then the Y2's). Remove
the B6 placeholders then (B7 §17).

**Build 5 on the Y1 — PASS** (flashed by the owner; the overrides in the running root equal the repository's):

- Boot: Bluetooth powered, the Wi-Fi device registered and blocked, Wi-Fi off as left. Running: `wpa_supplicant`,
  radiod and the monitor; no readiness supervisor (beta 9.3.12's platform lacks the policy, so the guard
  holds). `y2-calibration` is still restarted, as expected with that root.
- **The late Wi-Fi record**: the record and `WIFI.defaults` were put aside on Y2DATA, then a reboot. Activation
  registered Bluetooth only: no Wi-Fi device, supplicant or monitor. Both files back, then S41 stop/start (what an
  import does): `Wi-Fi registered after activation`, rfkill `y2-wifi` blocked, the supplicant and monitor
  started, Bluetooth powered, no error. Wi-Fi on: the same address as before (the record's MAC), `country AT`,
  gateway and DNS. Then off again (`/data/network/enabled` 0).

**Open**: W5b (throughput with one access point, an upload, Bluetooth audio during a transfer), the platform part
on the device (next Y2ROOT), the action hook's pid file.
