# Y1 audio: ALSA playback through the MT6572 AFE and the MT6323 codec

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The step builds named
> below (the *N*th boot, its variant, tree, fragment and PID1 header) are the record of the test images that ran
> on the player; their files are folded into that build.

Status: jack detection **PASS** (2026-11-09, [§14](#14-jack-detection--2026-11-09)). **stage B PASS** (2026-11-05, [§10](#10-stage-b-run-1--headphone-playback-pass--2026-11-05)): ALSA
headphone playback on the Y1 through the MT6572 AFE and the MT6323 codec. **Stage C PASS** (2026-11-05,
[§13](#13-stage-c-run-1--2026-11-05)): the speaker through the OCA72559 amplifier, switchable against the
headphones. The owner chose audio before data and
power (2026-11-05). Follows the [SMP plan](Y1-SMP-PLAN.md) (PASS) and the [system plan](Y1-SYSTEM-PLAN.md)
(the Y2's userspace runs; it has `aplay`, `speaker-test`, `amixer`).

## 1. Goal

Sound from the Y1 under Linux through standard ALSA: first the headphones, then the speaker. Not in scope:
recording, Bluetooth audio, the Y2's player, jack detection beyond what playback needs, suspend.

## 2. Evidence

### Existing code (2026-11-05)

- **Mainline 6.18 and the mt6572-mainline fork:** no MT6572 audio front end (AFE), no MT6323 codec driver
  (only newer PMICs: mt6351/57/58/59), no AW87559 driver. The fork has no audio work at all.
- **The Y2** (`kernel/audio/`): `mt6582-afe.c` (DL1 memif, own DPCM front/back end, 44.1 kHz S16, IRQ
  handling) drives a **second I2S to an external CS43131 DAC**; the MT6323's own codec is not used.
  `aw87559.c` drives the same AW87559 amplifier (I2C, enable GPIO, the stock 39-register table).
- **Reusable for the Y1:** `aw87559.c` with Y1 values (I2C0 `0x58`, enable GPIO143); most of
  `mt6582-afe.c` (the MT6572 AFE has the same register offsets per the vendor `AudDrv_Afe.h`: DL1
  `0x40`–`0x48`, IRQ `0x3a0`–`0x3ac`) with a different base and, instead of I2S, the **internal ADDA path**
  to the PMIC. **To be written:** an MT6323 codec component (pwrap regmap, MFD child).

### Stock 3.0.2 kernel (offline)

- AFE at `0x11140000` (SRAM `0x11141000`), IRQ 96 = SPI 64, level low; clock `MT_CG_AUDIO_SW_CG`
  (topckgen CG1 bit 25, mainline `CLK_TOP_AUDIO`); `AUDIO_TOP_CON0 = 0x60004000` at clock-on.
- The kernel is a pass-through (`/dev/eac`): the closed HAL writes AFE registers and MT6323 registers
  (`Ana_Set_Reg` over pwrap) by ioctl. The kernel itself only sets `AUDTOP_CON5/6` once at probe and drives
  the AW87559 on the HAL's `SET_SPEAKER_ON/OFF` (GPIO143 high, 50 ms, init table, SYSCTRL `0x78`; off:
  SYSCTRL `0x30`, GPIO low). PMIC audio registers: `ABB_AFE_CON0-11` `0x4000`–`0x4016`, `NEWIF_CFG0-3`
  `0x4024`–`0x402a`, `AUDTOP_CON0-9` `0x700`–`0x712`, `SPK_CON` `0x52`–`0x6a`, clock gates `0x102`–`0x126`.
  Jack: MT6323 ACCDET (`0x77a`–`0x790`) with EINT0 on GPIO135.
- **The sequences (power-up, depop, rate, routing, gains) are in the HAL**, which the backup holds:
  `libaudio.primary.default.so` (528 KB, exported C++ symbols, sha256 `af869d8f…bf6`), extracted from the
  backup's `ANDROID` image into `evidence-private/2026-11-05-y1-audio-hal/`.

## 3. Stages

- **Stage 0 (offline, done, §8):** the HAL's exact register sequences for playback, the headphone and speaker
  power-up/down, rates, routing and gains.
- **Stage A (read-only, device):** the AFE, PMIC audio, clock-gate and ACCDET registers as LK leaves them,
  from the running Y1 (SSH: `devmem` for the AFE after checking its clock gate; the PMIC through the
  read-only regmap file). The baseline for stage B.
- **Stage B (first sound, headphones):** AFE driver (from the Y2's, MT6572 base/IRQ/clocks, ADDA back end
  with §8's interconnect and DL SRC2 values), MT6323 codec component with §8's headphone sequence (DC trim 0,
  see §8) and the lowest analog gain, a sound card at 44.1 kHz S16 stereo (48 kHz differs only in the rate
  codes); the PMIC wrapper's write policy extended to exactly the registers of §8 (`0x10a`/`0x10c` bit 8,
  `0x4000`–`0x4016`, `0x4024`, `0x700`, `0x708`, `0x70a`, `0x70c`). Test: a quiet `speaker-test` tone, the
  headphones away from the ears first.
- **Stage C (speaker):** the headphone path on, then the AW87559 (shared driver) on, as stock: the speaker
  amplifier is fed from the MT6323's headphone outputs (the PMIC's own speaker amplifier is never used); off
  in the reverse order; low level first.

## 4. Risks

Loud pops or full-scale output into headphones (the test starts at low digital gain, headphones away from the
ears for the first tone); a wrong analog sequence (wrong bias, DC on the output) — bounded by reproducing
stock exactly; the speaker amplifier driven hard (stage C only, low level first). The AFE reads stage A does
are gated on its clock being on.

## 5. Later

Jack detection (ACCDET), volume control mapped to the PMIC gains, 48 kHz if the path supports it, the Y2's
player (milestone E).

## 6. Admission for stage A (now)

| Item | Bound |
| --- | --- |
| **Read the audio registers** (AFE `0x11140000`, PMIC audio and ACCDET ranges) | read-only (`devmem` reads after the AFE clock-gate check; the regmap file for the PMIC); nothing written |

## 7. Admission for stages B and C (later, with stage 0 and A evidence)

| Item | Bound |
| --- | --- |
| AFE playback (memif, interconnect, ADDA) | stock values only |
| MT6323 codec writes | the HAL's headphone sequence; the wrapper policy allows only those registers |
| AW87559 on, speaker | the stock table; low digital level first |

## 8. Stage 0 result: the stock sequences — 2026-11-05

Offline analysis of the stock HAL (`libaudio.primary.default.so`, ARM A32; notes
`evidence-private/2026-11-05-y1-audio-hal/y1-audio-hal-notes.md`). CONFIRMED from code unless marked.

- **Format:** primary output 44.1 kHz, stereo, 16-bit (`audio_policy.conf`, the HAL's defaults); no
  resampling; DL1 buffer 16 KiB, IRQ every 2048 frames. 48 kHz differs only in the rate codes.
- **Rate codes:** AFE fs 44.1k = 9, 48k = 10 (`DAC_CON1`, `IRQ_MCU_CON`, `I2S_CON1`, PMIC `0x4002`); NEWIF
  44.1k = 7, 48k = 8; `ADDA_DL_SRC2_CON0` 44.1k `0x73001803`, 48k `0x83001803`; `DL_SRC2_CON1` `0xf74f0000`.
- **AFE start** (`AudioMTKStreamOut::write`): the kernel's clock and buffer (it owns `AUDIO_TOP_CON0` and
  `DL1_BASE/CUR/END`); `IRQ_MCU_CON 0x3a0` fs, `IRQ_MCU_CNT1 0x3ac = 2048`, IRQ on; interconnect
  `CONN1 0x24` bit 21 (I05→O03), `CONN2 0x28` bit 6 (I06→O04); `0x260 = 0`, `0x264 = 0`; `0x108` rate value,
  `0x10c = 0xf74f0000`, `0x34 = fs<<8 | 8`; enable `0x108` bit 0, `0x34` bit 0, `UL_DL_CON0 0x124` bit 0,
  `0x4c4` bit 4 = 0; `DAC_CON0 0x10` bit 0; the analog power-up; `DAC_CON1 0x14` fs, `DAC_CON0` bit 1 (DL1
  on). Init also writes `0x4b8 = 0x0300f872`, `0x4bc = 0x9331802f`, `0x4c0 = 7`, `0x4c4 = 0`. Playback never
  writes NEWIF `0x138/0x13c` or `ADDA_TOP_CON0`. Stop is the reverse.
- **MT6323 headphone power-up**, digital then analog: `0x10c` ← `0x100` (clock gate clear, INFERRED),
  `0x4024` = `newif<<12 | 0x330`, `0x4002` = rate, `0x4006`/`0x4008` = left/right DC trim, `0x4014` bit 0,
  `0x4016` bits 9:8 toggled, `0x4000` bit 0; then `0x70c = 0xF7F2`, `0x700` bits 15:12 = 7, `0x70a = 0x0014`,
  `0x708 = 0x007C`, 10 ms, `0x70c = 0xF5BA`, `0x70a = 0x2214`. **Down:** `0x70a = 0x0014`, `0x70c = 0xF7F2`,
  `0x708 = 0`, `0x700` bit 12 off (no uplink), `0x70c = 0x37E2`, `0x4000` bit 0 off, `0x10a` ← `0x100`.
- **Volume:** `AUDTOP_CON5 0x70a` bits 14:12 and 10:8, 3-bit fields, 7 = loudest, 2 dB steps (scale
  INFERRED).
- **DC trim:** no efuse; stock measures the offset once through AUXADC channels 15/14 into NVRAM and writes
  1.8 × that to `0x4006`/`0x4008` (mishandling negative values). Stage B writes 0 (no trim) and may add a
  measured trim later.
- **Speaker:** the MT6323's own speaker amplifier is never enabled; the speaker path is the headphone power-up
  then the kernel's AW87559 on; close is the reverse. The volume uses the same `0x70a` fields.

## 9. Stage A result and stage B build — 2026-11-05

**Stage A** (owner-approved with stage B; `evidence-private/2026-11-05-y1-audio-a/regs.txt`): topckgen
`CG_CON1` = `0xDAC9DAFF`, bit 25 set, so **the AFE's clock is gated after LK** and the AFE was not read
(by design). The PMIC audio registers are idle: `0x108 = 0x03bf` (bit 8: the audio clock gated; `0x10a`/
`0x10c` are its set/clear registers), `0x4000 = 0`, `0x4002 = 0`, `0x4014 = 1`, `0x4024 = 0x9330`,
`0x4028`/`0x402a` = `0x302f`/`0xf872` (the halves of the AFE init values stock writes), `0x700 = 0x6010`,
`0x708 = 0`, `0x70a = 0x1100`, `0x70c = 0x1c32`, ACCDET `0x77a = 0x0010` (off).

**Stage B build:**
- `kernel/y1/mt6572-afe.c` (added as `sound/soc/mediatek/y1-mt6572-afe.c`): one CPU DAI "DL1-ADDA" (no
  DPCM): the Y2 AFE driver's IRQ, DMA and pointer handling; probe enables `CLK_TOP_AUDIO`, writes
  `AUDIO_TOP_CON0 = 0x60004000`, the stock reset and init values; `prepare` the stock ADDA path and AFE on;
  `trigger` IRQ and DL1 on; `hw_free` the reverse. 44.1 kHz S16 stereo.
- `kernel/y1/mt6323-codec.c` (added as `sound/soc/mediatek/y1-mt6323-codec.c`, MFD cell in `0014`): the
  stock headphone power-up at `prepare` (DC trim 0, final gain from the mixer), power-down at `hw_free`;
  "Headphone Playback Volume" 0–7, default 0 (lowest); nothing written at probe.
- `kernel/y1/pmic-audio-policy.h` (added to `drivers/soc/mediatek/`): the wrapper's policy admits the 12
  audio registers with exact value guards (e.g. `0x70c` only `0xF7F2`/`0xF5BA`/`0x37E2`, `0x4024` only
  `0x7330`, `0x70a` only `0x0014` plus the gain fields, `0x10a`/`0x10c` only `0x100`); overlay `0005`.
- `0028`/`0029`: `SND_SOC_Y1_AUDIO`. Tree `innioasis-y1-thirteenth-boot.dts`: the AFE (SPI 64 level low,
  `CLK_TOP_AUDIO`), the codec under the PMIC, a `simple-audio-card`. Config: `SOUND`, `SND`, `SND_SOC`,
  `SND_SIMPLE_CARD`, `SND_SOC_Y1_AUDIO`.
- Test tones (`out/y1-audio/`): 440 Hz, 3 s, 100 ms fades, at −40, −30 and −20 dBFS.

Build `out/y1-thirteenth-boot-1`, BOOTIMG sha256
`243db0ace5af2ef98be8d6f4809bcd85a0d9337d8d05eb613422f0f0c0f77e15`, no compiler warnings. Physical status
NOT_RUN.

## 10. Stage B run 1 — headphone playback PASS — 2026-11-05

Build `243db0ac…`; `evidence-private/2026-11-05-y1-audio-b/`. Card `Y1`, device `DL1-ADDA-mt6323-hifi`;
"Headphone Playback Volume" 0–7. During a −40 dBFS 440 Hz tone the registers were the stock sequence
(PMIC `0x108` bit 8 clear, `0x4000 = 1`, `0x4002 = 9`, `0x4024 = 0x7330`, `0x700 = 0x7010`, `0x708 = 0x7c`,
`0x70c = 0xF5BA`; AFE `CONN1` bit 21, `CONN2` bit 6, `DL_SRC2 = 0x73001803`, `UL_DL_CON0` on,
`DAC_CON0 = 0x3003`, IRQ at fs 9, `DL1_CUR` inside the ring); `aplay` without underrun; afterwards the stock
off state (`0x70c = 0x37E2`, `0x708 = 0`, `0x700 = 0x6010`, the audio clock gated). Charging unaffected.

The owner, headphones: at gain step 0 and −40 dBFS nothing audible; at step 2 the tone; steps 2, 4, 6 louder
each time, no distortion; `speaker-test` left and right on the correct sides; very small pops before the
start and after the end (no DC trim, and the stock sequence has no ramp; as stock without its NVRAM trim,
INFERRED). **Stage B: PASS.**

Side findings (the HAL follow-up, `y1-audio-hal-notes.md`): the stock playback path writes no other AFE or
PMIC register; the AFE writes at `0x4b8`–`0x4c4` read back 0 on the Y1 (not retained, meaning unknown,
harmless); stock sets `DAC_CON0` bit 10 only on restarts and `0x4c4` bit 4 at standby. The headphones were
first kept away from the ears; the pin mux (GPIO4–6 mode 1, AUD_CLK/MISO/MOSI) is as LK left it.

Next: the default gain (step 0 is inaudible; stock starts at 2), volume mapping, the pops (a measured DC trim,
a ramp), 48 kHz, jack detection, then stage C.

## 11. Stage C plan: the speaker — 2026-11-05

**Stock** (stage 0): the MT6323's own speaker amplifier is never used; the speaker path is the full headphone
power-up, then the kernel's AW87559 on (GPIO143 high, init table, SYSCTRL `0x78`, 80 ms settle); off is
SYSCTRL `0x30`, GPIO143 low, then the headphone power-down. The AW87559 is fed from the MT6323's headphone
outputs (INFERRED from the code; the board wiring is not visible). Stock's ID check accepts `0x5A`
(AW87559) or `0x09` (OCA72559).

**Design:**
- The Y2's `kernel/audio/aw87559.c` (shared, unchanged; the same 39-register table as the Y1's stock
  `aw87559_hw_init`; enable pulse low 10 ms, high, 100 ms; ID check `0x5A`/`0x09`; a DAPM output driver
  "DRV" that switches it on after power-up and off before power-down, 80 ms settle) on **I2C0 `0x58`,
  enable GPIO143**, as an aux device with the prefix "PA". I2C0 is enabled for it (the second boot left it
  off; nothing else is on that bus in the tree).
- The MT6323 codec's headphone power-up/down moves from `prepare`/`hw_free` into a DAPM DAC event, with an
  output "HPOUT": DAPM then powers the DAC before the amplifier and the amplifier down before the DAC, the
  stock order in both directions.
- `simple-audio-card` widgets "Headphones" and "Speaker", routes `Headphones <- HPOUT`, `PA IN <- HPOUT`,
  `Speaker <- PA OUT`, and pin switches for both: `amixer cset name="Speaker Switch" off|on` chooses.
- The default gain becomes step 2 (stock's power-up value; step 0 was inaudible).

**Test:** the headphones unplugged; "Speaker Switch" on, "Headphones Switch" off; the −40 dBFS tone at gain
step 2, then −30 dBFS; then the speaker off and the headphones as in run 1 (no regression); the AW87559's ID
read in the kernel log.

**Admission (stage C):**

| Item | Bound |
| --- | --- |
| **I2C0 bus** | enabled for the amplifier at `0x58` only |
| **The AW87559 on and off** (GPIO143, its registers) | the shared driver's stock table and order; only while a stream routes to "Speaker"; low level first (−40 dBFS, gain step 2) |
| **The codec's power-up moves into DAPM** | the same register sequence and write policy; only its timing relative to the amplifier changes, to stock's order |

## 12. Stage C build — 2026-11-05

- `kernel/y1/mt6323-codec.c`: the headphone power-up/down is the event of a DAPM DAC "HP DAC" (stream
  "Playback", `PRE_PMU`/`POST_PMD`) with the output "HPOUT"; the DAI has no ops; default gain step 2.
- The shared `kernel/audio/aw87559.c`, unchanged, added as `sound/soc/mediatek/aw87559.c`;
  `SND_SOC_Y1_AUDIO` now depends on `I2C` and selects `REGMAP_I2C` (overlays `0028`/`0029`).
- `innioasis-y1-fourteenth-boot.dts`: I2C0 on (100 kHz) with `amplifier@58` (`awinic,aw87559`,
  `enable-gpios` GPIO143, prefix "PA"); the card's widgets "Headphones"/"Speaker", routes
  `Headphones <- HPOUT`, `PA IN <- HPOUT`, `Speaker <- PA OUT`, pin switches for both, the amplifier as aux
  device. Config: the thirteenth fragment.

Build `out/y1-fourteenth-boot-1`, BOOTIMG sha256
`42bf0daf5a1fa6832ccf4c37305ca072ee4d5dd4ad28a4b37fec7184815f2507`, no compiler warnings. Physical status
NOT_RUN.

## 13. Stage C run 1 — 2026-11-05

Build `42bf0daf…`; `evidence-private/2026-11-05-y1-audio-c/`. The amplifier answered on I2C0:
`aw87559 0-0058: OCA72559 (chip id 0x09)`: the owner's Y1 has the **OCA72559** variant, which the shared
driver drives with its own table (stock does the same). Controls: "Headphones Switch", "Speaker Switch",
"Headphone Playback Volume". Speaker only (headphones off, gain step 2): the owner heard both the −40 dBFS and
the −30 dBFS tone from the speaker. No kernel error; charging continued. Headphone recheck (speaker switch off,
headphones on, gain step 2, −40 dBFS): heard in the headphones, the speaker silent. **Stage C: PASS.**

## 14. Jack detection — 2026-11-09

**Evidence.** Stock (§2): ACCDET with its plug switch on EINT0, GPIO135 mode 4, no pull, 256 ms debounce,
polarity 0. On the owner's Y1 (single-build era, the thirty-fifth build running; read-only `devmem`): GPIO135 is
in mode 4 as LK leaves it; its input bit (`0x10005240` bit 7) reads **1 with the jack empty and 0 with
headphones in**, three changes each way, one bounce while re-seating the plug. The plug switch alone is enough to
pick headphones or the speaker; the ACCDET block (headset type, hook key) stays unused.

**Design.** The Reborn interface is the Y2's: the card's jack "Headphone" (input "Y2Audio Headphone",
`SW_HEADPHONE_INSERT`).
- `kernel/mt6572/.../pinctrl-mtk-mt6572.h`: pin 135 carries EINT0 in mode 4 (pin 30, the other EINT0 pin, is
  not claimed).
- `kernel/y1/mt6323-codec.c`: `hp-det-gpios` (GPIO135, active low). At card bind the jack is created and read at
  once, then each edge restarts a 250 ms quiet period before the next read. On a plug while the "Speaker" pin is
  on, the HPOUT gain first drops to step 0 (the amplifier is fed from HPOUT, so the headphones would otherwise
  hear the speaker's gain) and the speaker pin goes off, as the Y2's kernel does; Reborn then moves the stream.
- `innioasis-y1.dts`: the codec's `hp-det-gpios = <&pio 135 GPIO_ACTIVE_LOW>`.
- Reborn `crates/reborn-platform/src/jack.rs`: without the Y2's poll state file (`*-0030/y2_hp/jack`) the jack
  switch is believed from the start (the Y1 reads it before the jack input exists).

**Build** `out/y1-jack-1` (the single build plus the above), BOOTIMG sha256
`ac5a8ab34b571d7139022b34b0dc82b08e449070d29be1be24a0aec31d40fa85`; no new compiler warning. Root
`out/y1-root-jack` (the working tree with Reborn 9.3.19 and the `jack.rs` change), `rootfs.ext4` sha256
`3f9a2023bc4f7f6deca9f4a99159c11500a3b3704aac2bc185856d67c1e3d29a`.

**Run 1 (2026-11-09).** The first write put `BOOTIMG.img` into ANDROID (a mistyped partition): build 5 booted
and stopped without a root (`loop0: Can't find ext4 filesystem`); the rescue shell showed `ANDROID!` at the start
of Y2ROOT, matching the new BOOTIMG. With the root above in ANDROID and the build in BOOTIMG the Y1 boots, and the
owner reports the jack detection working: **PASS** (owner-observed; no log kept).

**Test.** `/proc/interrupts` lists `y1-headphone-jack`; `cat /sys/class/input/*/name` lists
"Y2Audio Headphone"; plug and unplug with Reborn's Speaker setting on: music moves to the headphones on a plug
(the kernel log says `speaker off`) and pauses on an unplug; idle with the jack empty, playback starts on the
speaker; with headphones in, in the headphones.

### 14.1 Reborn reads the jack as plain ALSA — 2026-11-09

**Why.** The owner wants no hardware-dependent jack code in userspace. Reborn found the jack by the input
device name "Y2Audio Headphone" and read the Y2-only `*-0030/y2_hp/jack` file to learn whether the CS43131
poll had read the detector yet; the Y1 needed its own case for that file (Reborn `b23f8dd`).

**Change.** Both kernels already create the jack with `snd_soc_card_jack_new()`, which adds the card's ALSA
control "Headphone Jack" (iface CARD, read-only boolean, a change event on each report: what PipeWire and
PulseAudio read).
- Y2 kernel (`0030-cs43130-system-pm.patch`, `y2_jack_get`): the poll's first read powers the codec itself
  if it is down (`pm_runtime_resume_and_get`), so the reported state is never a guess; after that it reads
  only while the codec is on, as before. In practice the codec is still on then (powered at I2C probe,
  120 s autosuspend; the first poll runs 200 ms after the card binds), so this only covers a late bind.
- Reborn `jack.rs`: the first card (`/dev/snd/controlC*`) with a "Headphone Jack" control; its value
  (`SNDRV_CTL_IOCTL_ELEM_READ`) at start and after each value event (`SUBSCRIBE_EVENTS`). The structure
  sizes and ioctl numbers are checked against `<sound/asound.h>` (arm-linux-gcc: 712 bytes; x86_64: 1224).
  No input device name, no sysfs, no I2C bus search. `y2_hp/jack` stays for diagnostics.

**Test (needed on both).** `amixer -c 0 cget iface=CARD,name='Headphone Jack'` follows plug and unplug, and
Reborn behaves as in the §14 test (speaker when empty, a plug moves the music to the headphones, an unplug
pauses). The Y2 additionally right after boot with the screen off: the state must already be right.
