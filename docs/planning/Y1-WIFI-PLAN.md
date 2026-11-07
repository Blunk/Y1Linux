# Y1 Wi-Fi

Status: **W0–W2 done 2026-11-07** (owner-approved): the firmware source decided (F2, the owner's Y2 file, by hash
comparison, §2), the Y1's Wi-Fi record found (§7), the driver extended and built, not loaded (§8); W3–W6 not admitted. The Y1 ships with Wi-Fi disabled and no Wi-Fi firmware. New hardware scope (a
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
