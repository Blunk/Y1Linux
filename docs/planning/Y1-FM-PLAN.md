# Innioasis Y1: FM radio — evidence and plan (2026-11-14)

**Status (2026-11-15): FM plays on the owner's Y1** (§4; Reborn and the platform carry one fix found there). The owner asked on 2026-11-14 to port the Y2's FM radio (Beta 9.3.26,
[BETA-8-FM-PLAN.md](BETA-8-FM-PLAN.md), [FM-RADIO.md](../FM-RADIO.md)) to the `y1` branch. Audit:
[Y1 FM entry](roadmap-gap-audit.md#y1-fm-radio-entry-audit--2026-11-14). The Y1 shares the Y2's tuner driver,
capture sequence and Reborn screens; what differs is the radio core's supply, two WMT commands, the sound card
and where the firmware lives.

## 1. Evidence

All offline unless marked; the stock files are the public Innioasis Y1 3.0.2 package (`rom.zip` from the
y1-community stock ROM archive, release `3.0.2`, 259 502 414 bytes): its `system.img` (raw ext4) and
`/system/lib/modules`. Its `WMT_SOC.cfg` and `mt6572_82_patch_e1_0_hdr.bin` have the hashes of
[the Y1 inventory](../knowledge/evidence/y1-firmware-inventory.json), so it is the package the Y1 radio work used.

| Question | Answer | Source |
| --- | --- | --- |
| Does the Y1 have FM? | Yes: `MTK_FM_SUPPORT = yes`; stock ships `FMRadio.apk`, `mtk_fm_drv.ko`, `fmradio.driver.enable=1` | build config (Y1-WIFI-PLAN.md), `system.img` |
| The player (USB network, read-only) | `y1-connectivity 18070000.connectivity: CONSYS chip=6572 HVR=8a01 FVR=8a00`; no FM device, no FM firmware | Y1 `6.18.56-y1linux`, 2026-11-14 |
| Tuner firmware | `/etc/firmware/mt6627/mt6627_fm_v1_patch.bin` (1028 B, `ba35c74d…`) and `_coeff.bin` (1092 B, `d32f4a16…`): **byte-identical to the Y2's**; v2–v5 empty or 2 bytes; `/etc/fmr/mt6627_fm_cust.cfg` identical too | `system.img` |
| Analog or digital audio? | **Digital**: `libaudiosetting.so` `fm_use_analog_input = 0` (`.rodata`), so `FMAudioPlayer` sends `AudioSetFmDigitalEnable=1` | `libaudiosetting.so`, `libmtkplayer.so` |
| Tuner commands | Stock Y1 `mtk_fm_drv.ko` (older MT6627 driver) run in an emulator against `tests/fixtures/fm-stock-receive.json` (the Y2's): power-up 22 of 23 packets identical (digital init clears reg 0x30 bits 10–11 first); the same frequency, channel and desense values on all 411 channels; pre/restore search, ramp-down, volume, soft-mute tune, RSSI, CQI decisions, download, ROM version and the register builders identical. The Y2 driver additionally toggles reg 0x30 bit 11 per channel, sends the tune as separate packets, and switches the top clocks off and `0x80101054 = 0` at power-down. No `0x6572`/`0x6582` branch, no I2S, antenna or oscillator command | `mtk_fm_drv.ko` |
| FM on/off in WMT | Without co-clock (the Y1's `WMT_SOC.cfg`), `wmt_func_fm_on` sends only `01 06 02 00 01 01` (reply `02 06 01 00 00`), off `… 01 00`; **no rail**: VCN28 is the TCXO rail, on in hardware mode while the chip is (the Y2 is co-clock and switches VCN28 for FM) | `mtk_stp_wmt.ko` `wmt_func_fm_on` 0x4eb4, `mtk_wcn_consys_hw_reg_ctrl` 0x109b0 |
| FM strap | `01 05 02 00 02 02`, reply `02 05 02 00 00 02`, on every chip init after coex (value 2 hard-coded by `6620_launcher`): the "INFERRED 2" of Y1-BLUETOOTH-PLAN.md is CONFIRMED | `mtk_wcn_soc_sw_init` 0x6668, `wmt_core_init_script` |
| FM audio pins | Starting FM digital audio, the HAL's `AUDDRV_SET_FM_I2S_GPIO` ends in `mtk_wcn_soc_pin_ctrl` (AIF 2): CONSYS `0x80050078` bits 27:16 = `0x111` (value `0x01110000`, mask `0x0fff0000`), a WMT register write; off writes nothing (the dispatch from WMT op 17 to `pin_ctrl` INFERRED) | stock kernel 0xc0414d60, `mtk_stp_wmt.ko` 0x5678 |
| Capture sequence | Register for register the Y2's: `I2S_CON 0x18 = 0x8000000c` (slave), `0x4c4` bit 8 clear, `CONN4 0x30` bit 30 clear, `DAC_CON1` bits 11:8 = 10, `0x550` bit 16 clear, ASRC `0x554/0x558/0x560/0x55c/0x56c` = `0x600000/0x400000/0xcb2/0x75987/0x1b00`, `0x500 \|= 0x41`, I2S-in on, `CONN2` bits 16/22 (I00→O05, I01→O06), AWB (`0x70/0x78/0x7c`, `DAC_CON0` bit 6, rate `DAC_CON1` bits 15:12 = 10), IRQ2 (`0x3a0` bit 1, counter `0x3b0`), AFE on. 48 kHz stereo S16 (the HAL accepts only 48 kHz for `AUDIO_DEVICE_IN_FM`). No PMIC write | `libaudio.primary.default.so` `Do_input_start` 0x4e250 |
| Sleep | The Y1 refuses system sleep (Y1-REBORN-PLAN.md §16): FM keeps playing with the screen off; the sleep helper's FM step does not run | — |

Open (INFERRED, settled on the player): the FM core's I2S rate. The HAL's ASRC words assume 32 kHz from the
tuner, while the stock driver's `mt6627_i2s_info_get` reports a static 48000 and its I2S setting ioctl fails
(no op). The Y2 uses the same 32 kHz words.

## 2. What the port changes

- **Kernel, connectivity** (`kernel/y1/connectivity/`): the Y1 builds the Y2's `stp.c` again (it carries the FM
  channel; the copy of 050d9a37 is gone) and `fm.c` + `fm-protocol.h` unchanged (`y1-conn-fm.o`, overlay 0037);
  `conn.h` declares FM and lists the tuner's two files; `core.c` accepts the FM function without a rail, calls
  `y2_fm_lost` on a transport failure, registers the `fm` sysfs group in the full driver and powers the tuner
  down at shutdown; `wmt.c` sends the FM strap after coex, accepts function 1, and after FM on writes the audio
  pins (`y1_wmt_fm_audio`).
- **Kernel, audio** (`kernel/y1/mt6572-afe.c`): DAI 1 "FM" (capture, 48 kHz S16 stereo) with the sequence above;
  `AFE_ON` held while playback or capture runs; register map to `0x56c`. The tree's card has two links:
  `hw:0,0` playback unchanged, `hw:0,1` the FM capture with the generic `linux,spdif-dir` stub as codec side
  (`SND_SOC_SPDIF`).
- **Platform** (`radiofw/fm.rs`, `copy.rs`): the pair goes into the image's own firmware directory
  (`/data/firmware/mt6572` on the Y1); a later copy of either package's `system.img` supplies it (the Y1's
  firmware set rejects the Y2's image, so before this a Y2 copy was kept and FM never taken) and is then deleted.
- **Reborn** (`reborn-platform/src/fm.rs`): the firmware is looked for in the Y2's directory, else the Y1's; with
  no `/proc/asound` (the Y1's kernel has no `SND_PROC_FS`) the only capture node in `/dev/snd` is the tuner.
- **Tests**: `test_y1_connectivity` (shared sources incl. `stp.c` and `fm.c`; FM on = function + audio pins, no
  rail; off = function only), the inventory with the pair (`fm_tuner`), Reborn's capture fallback.

## 2a. Later: `/proc/asound` on the Y1

The Y1 kernel has no `/proc/asound` by accident: `y1-build.sh` runs `allnoconfig` over the fragment, and
`SND_PROC_FS` (default y, but a visible prompt once `EXPERT=y`) is not named there; the Y2's
`production.config` sets it. For now Reborn takes the only capture node in `/dev/snd` (§2). Planned (owner,
2026-11-14), after the FM test: `CONFIG_SND_PROC_FS=y` in `kernel/config/y1-production.config` (about 9 KB),
and with it Reborn's named lookup must find the Y1's FM link, whose PCM is named after its DAIs
(`FM-dir-hifi …`, not "Y2 FM Capture"): name the link to match, or let Reborn accept the Y1's name. The
`/dev/snd` fallback stays for kernels without it.

## 3. Device checks (owner's Y1)

1. Firmware: copy `system.img` of the Y1 3.0.2 or the Y2 3.2.0 package into `Firmware`; the pair appears in
   `/data/firmware/mt6572`, the copy is deleted, Bluetooth and Wi-Fi keep running.
2. Boot: `WMT opcode=05 … ok` (the strap) in the kernel log; Bluetooth and Wi-Fi as before.
3. Tuner: `echo 1 > …/18070000.connectivity/fm/power`: `FM on: chip=6627`, `functions` includes FM, WMT
   register write ok; `frequency`, `signal` (dBm across the band, with and without the headphone cable).
4. Capture: `arecord -D hw:0,1 -r 48000 -c 2 -f S16_LE -d 2`: a steady 96 000 frames; level and content against
   a known station (a 32 kHz/48 kHz mismatch would sound pitched).
5. Reborn: Radio shows FM Radio; search, play, pause (tuner off after the grace), screen off, Bluetooth output.
6. A station received on the Y1 (stock FM found stations on the owner's unit: owner to confirm).

If the tuner misbehaves where the Y2's newer driver differs (§1, tuner commands), the stock Y1 sequence is the
fallback.

## 4. On the owner's Y1, 2026-11-15

Images `out/y1-root-fm` (Y2ROOT, sha256 `284b4ce3…368e5d`) and `out/y1-boot-fm` (BOOTIMG `6223f233…5dd60f`),
ANDROID and BOOTIMG written with mtkclient, Y2DATA kept.

- **Firmware:** the Y1 3.0.2 `system.img` copied into `Internal Storage/Firmware` (over SSH; the owner's
  Nautilus MTP copy failed); the leftover pass took the pair into `/data/firmware/mt6572` (hashes as §1) and
  deleted the copy. Run by hand (`y2-platform firmware scan`): with the screen dark the service's maintenance
  pass comes every 300 s and a leftover needs two passes.
- **Boot:** the chip booted with the FM strap; Bluetooth and Wi-Fi as before (`functions=0x9`, no transport
  error): the strap is accepted.
- **Tuner:** `fm/power` 1: `WMT opcode=06 parameter=01 ok`, `opcode=08 … ok` (the I2S pin write), `FM on:
  chip=6627 rom=0 patch=0000` (ROM 0: the v1 pair); `functions=0xb`; off again leaves `0x9`.
- **Signal:** −99 to −103 dBm across the band. The search (soft-mute tune, 100 kHz) takes 7 s and lists 17
  frequencies, the strongest 102.8 MHz (−74 dBm), 88.5 (−80), 96.9 (−81) (the first-revision Y2 listed one or two
  probable false hits).
- **Capture** (`arecord -D hw:0,1`, 48 kHz): exactly 96 000 frames in 2 s; an empty frequency gives flat noise at
  −43 dBFS (left and right identical); 102.8 MHz gives −17.5 dBFS varying from −16 to −19 per 250 ms, mono. The
  owner: the recording "sounds very good": the 32 kHz ASRC words are right on the Y1 (§1, open point settled).
- **Reborn:** first "needs the tuner firmware": the Y1's Y2DATA still had the Bluetooth bring-up's placeholders in
  `/data/firmware/mt6582` (Y1-BLUETOOTH-PLAN.md, B6), which `/lib/firmware/mediatek/mt6582` links to, and Reborn
  took the first existing directory. Fixed in Reborn `482e6bb` (ready when either directory holds the pair); on
  the player the placeholders were moved to `/data/b6-placeholders` (the platform never read them; status stayed
  `Ready`). Then FM works in Reborn (owner). Y2ROOT rebuilt with Reborn `482e6bb` (`out/y1-root-fm2`, sha256
  `b62481e3…5b324e`), ANDROID written; FM works there too (owner), and the placeholders were deleted.
- **Not checked:** FM over Bluetooth by ear, long listening and battery use, the hide switch on the player, the
  pair from the Y2 package's image on a Y1 (the branch exists; this run used the Y1's image).

