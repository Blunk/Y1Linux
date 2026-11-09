# Y1 player UI: Y2Reborn (system milestone E)

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The step builds named
> below (the *N*th boot, its variant, tree, fragment and PID1 header) are the record of the test images that ran
> on the player; their files are folded into that build.

Status: **planned** (2026-11-05), not admitted. The owner asked to run Y2Reborn after the power-off PASS
(D2 and D3 of [the power plan](Y1-POWER-PLAN.md) and milestone B are not done). Each stage has its own admission.

## 1. Evidence

Read-only: the release-220 rootfs (the one the Y1 runs), `strings` of `/usr/bin/reborn` 1.0.0-beta.9.3.12
(the Y2Reborn source, `../Y2Reborn`, is not on this machine), the Y2 tree and the Y1 over SSH. Agent notes:
`out/y1-reborn-notes.md`. "CONFIRMED" means seen in a file; "INFERRED" is marked.

- **Already on the Y1** (SSH, fifteenth boot): `/usr/bin/reborn`, `rebornctl`, `reborn-supervise`,
  `reborn-splash`; 487 MB RAM, 441 MB free; `/dev/dri/card0`, two input devices, the `Y1` sound card. Root is the
  RAM overlay (32 MiB upper layer), no `/data`. `/proc/asound` is absent (ALSA proc interface off).
- **Rendering is the blocker** (CONFIRMED): Reborn draws only through DRM/GBM/EGL/GLES2; the image's Mesa has the
  `lima` and `kmsro` drivers and no software renderer. The Y2 renders on its Mali-400 with Lima
  (`docs/knowledge/gpu-platform.md`; Y2 patches 0038 and 0039). The Y1 kernel has no GPU driver.
- **The MT6572's GPU** (the fork's `mt6572-fork.dtsi`): Mali-400 at `0x13010000` (GP, GP MMU, PP0, PP0 MMU;
  SPI 114–117), clocks from `mfgcfg` `0x13000000` (`CLK_MFG_BG3D`), the SPM power domain `MFG` (clock
  `CLK_TOP_MFG_MM`), reset `MT6572_TOPRGU_MFG_SW_RST`, an OPP table 400/600/800 MHz. None of these is used in the
  Y1 tree yet; the Y1's display runs in the power state LK left. Linux has never written the Y1's SPM.
- **Display fits** (CONFIRMED): Reborn expects 480x360 XRGB8888 on a KMS driver named `mediatek`; the Y1's
  DRM pipeline is the same.
- **Start-up** (CONFIRMED): `S90reborn` exits unless `/data` is a mountpoint with `.y2data-schema` = 1;
  `boot-gate` waits up to 45 s for the power daemon's socket and files and D-Bus; `reborn-supervise` allows 5
  exits a minute, then shows a failure screen, saves a report and, after 600 s on battery, powers off. Reborn
  refuses mutable data unless Y2DATA is ready; music comes from `/data/music`, `/media/internal/music` or
  `/media/sd`. Internal media and SD are tied to the Y2's MMC controllers, so they are never admitted on the Y1.
- **Input** (CONFIRMED by name, INFERRED in effect): Reborn picks devices by name (`APT32F click-wheel`,
  `Y2 navigation buttons`, `mtk-pmic-keys`, …) and expects KEY_UP/DOWN (wheel), KEY_LEFT/RIGHT, KEY_ENTER (select
  on the navigation buttons). The Y1's wheel zones send PLAYPAUSE/NEXTSONG/BACK/PREVIOUSSONG and its centre
  KEY_ENTER comes from `mtk-pmic-keys`, which Reborn treats as the power key: the centre button may act as
  power, with no select.
- **Audio** (CONFIRMED): Reborn opens `hw:CARD=Y2Audio` and uses the Y2's CS43131 controls; the Y1's card is
  `Y1` with `Headphone Playback Volume` 0–7 and two switches, and no jack detection.
- **Memory** (CONFIRMED, Y2): 22–24 MB idle, about 92 MB playing a long audiobook.

## 2. Stages

| Stage | What | Image |
| --- | --- | --- |
| **R0** | Review the Y2Reborn source: input mapping, the Y2DATA readiness test, behaviour without the power daemon and with a different card, whether a non-Y2 device can be recognised cleanly | none (needs the source) |
| **R1** | GPU: read-only baseline (MFG power and clock state), then the MFG power domain, `mfgcfg` clocks and Lima; checked with the image's `y2-gpu-check` and a GLES test | new boot |
| **R2** | First UI, nothing persistent: a Y1 start script, a RAM `/data`, a few tracks copied over SSH, the Y1 input mapping, audio by a decision from R0 | new boot / overlay |
| **R3** | Persistent `/data` (milestone B, Y2DATA), then the power daemon (power plan D2), then the stock start chain | later plans |

R1 is the hardware risk; R2 is software on top. R0 first, because an R2 that guesses at Reborn's behaviour
can do things nobody intended (a power key on the centre button, a gain change without jack detection).

## 3. R1 — the GPU

- **Baseline** (reads only): SPM `MFG` power status, `mfgcfg` clock gates, the topckgen MFG mux; confirms LK
  leaves the GPU off (expected).
- **Kernel:** the fork's MT6572 SPM power-domain data and `mfgcfg` clock driver ported as `patches-y1`
  overlays (only the `MFG` domain described; `DIS` stays as LK left it), `DRM_LIMA` with the Y2's patch 0038
  (runtime idle), the fork's GPU node at the lowest OPP that the clock tree gives without reprogramming a PLL.
  Lima uses its own MMU, so no CMA.
- **Writes:** the SPM `MFG` power sequence (key, power on, isolation, SRAM), the `mfgcfg` clock gates and the
  MFG reset. They are the first SPM writes from Linux on the Y1, so the sequence is checked against the stock
  kernel's `spm_mtcmos_ctrl_mfg` before the build.
- **Test:** Lima probes ("Mali-400 MP1"), `y2-gpu-check` and a GLES render on the panel, the GPU idles off
  (runtime PM), display, audio, charging and SSH unaffected; a hang ends in the watchdog reset.

## 4. R2 — first UI (outline; detailed after R0)

A Y1 start script in `initramfs/y1-system` (not `S90reborn`): a RAM `/data` (tmpfs, or an ext4 image in
RAM if Reborn requires one) with the schema file and `/data/music`; Reborn under a Y1 supervisor without the
45 s gate, the timed power-off and report saving; the Y2 power, readiness, media, platform and radio services
stay off; the Y1 wheel and centre mapped to Reborn's keys (device tree codes or names, from R0); audio either
absent or the `Y1` card exposed the way R0 shows is safe (gain capped).

## 5. Risks

SPM and GPU power writes on an unverified sequence (R1 compares with stock first; recovery by the watchdog and
restoring the fifteenth boot). Lima on the MP1 variant at the MT6572's clocks (the Y2 is MP2). Reborn behaving
unexpectedly without its platform (R0). Memory: the RAM overlay is small; `/data` and music in RAM take from
the 441 MB free.

## 6. Out of scope

Y1 internal media or SD storage, updates (`y2-update-core`), sleep/suspend, Bluetooth and Wi-Fi, GPU
frequency scaling.

## 7. Admission

| Stage | Item | Bound |
| --- | --- | --- |
| R0 | **Review the Reborn source** | reading only; needs the owner to provide `../Y2Reborn` |
| R1 | **Power the GPU** | baseline reads first; then only the SPM `MFG` domain, the `mfgcfg` gates, the MFG reset and the Mali-400 registers; no PLL change, no other SPM domain |
| R2 | **Start Reborn** | RAM-only data; no Y2 storage, update, sleep, radio or power-daemon service; audio and input as R0 decides |

## 8. R0 result — 2026-11-05

Owner-approved. The owner placed the source at `../Y2Reborn` (checked out at `baseline-215`, 1.0.0-beta.9.3.7); the
review read tag `baseline-220` (`b55ecbc6`, 1.0.0-beta.9.3.12), the version on the Y1, from a `git archive` copy
(the checkout untouched). Notes with file:line: `out/y1-reborn-r0-notes.md`. All CONFIRMED in the source unless marked.

- **Input** (`crates/reborn-platform/src/input.rs`): devices by exact name only. Select (KEY_ENTER), Back,
  Left/Right and Play/Pause count only from `Y2 navigation buttons`; Power only from `mtk-pmic-keys`; KEY_NEXTSONG,
  KEY_PREVIOUSSONG (and KEY_PLAYCD/PAUSECD/SPACE) from any device as track keys; wheel rotation as
  KEY_UP/DOWN from `APT32F click-wheel` or `REL_WHEEL` from any device. On the Y1 today: rotation and the
  next/previous zones work; the centre, Back and Play/Pause zones are ignored (the centre never acts as power).
  **Only Power wakes the screen** after the 60 s default timeout, and the Y1 has no Power key.
- **Data** (`storage.rs`, `main_loop.rs`): `/data` must be in `/proc/mounts` as **ext4** with `.y2data-schema`
  = 1 and no maintenance-pending file; no label or device check. A tmpfs fails; an ext4 loop image in RAM passes.
- **Platform**: nothing missing is fatal. Power off and Restart in the UI need `/run/y2/power.sock` (no
  `/sbin/poweroff` fallback, because the rootfs carries `/etc/y2linux/platform-contract`), so they fail with an
  on-screen error. The idle sleep request after a dark screen is refused by the platform on the Y1 (no SPM sleep
  interface). No automatic power-off, update or mkfs in Reborn itself; the 600 s battery power-off is in the
  rootfs `reborn-supervise` script.
- **Rendering** (`graphics.c`): the first DRM card named `mediatek`, GBM linear XRGB8888, EGL/GLES2, and the GL
  renderer must contain "Mali400"; no fallback (a graphics failure is not fatal, Reborn runs on blind).
- **Audio** (`audio.c`): card id `Y2Audio` only; without it audio is "unavailable", not fatal. Stream open
  requires a `Master` element with a dB range and a `Headphone`/`Speaker` switch; the speaker is used only
  when a jack device reports empty. The Y1 card renamed as it is would fail every stream (INFERRED: no `Master`).
- **No board detection**: Y2-only parts are absent on the Y1, not refused.
- **Manual start**: `/usr/bin/reborn`, no arguments, root, umask 077, once `/data` is ready; logs in
  `/data/reborn/logs`. The stock supervisor cannot be used on the Y1 (power-off, report save).

**R2 decisions from R0** (part of the R2 admission): `/data` a 64 MiB ext4 image on its own tmpfs, loop-mounted;
input by two small kernel renames (the wheel's zones on a second input device named `Y2 navigation buttons`
with KEY_PLAYPAUSE, KEY_RIGHT, KEY_BACK, KEY_LEFT; the MT6323 key device renamed `Y2 navigation buttons`, so the
centre is Select), no Reborn rebuild; `screen_timeout_seconds` seeded as 0 in `/data/reborn/state/session.json`
(if the owner sets a timeout in Settings, only a Reborn restart over SSH wakes the screen); no sound card for
Reborn at first (a `Master` dB control, a headphone switch and a capped gain are a later audio stage); a Y1
start script with a bounded restart loop, no boot gate, splash, report save or power-off.

## 9. R1 research and baseline — 2026-11-05

Owner-approved (R0 and R1). Agent notes `out/y1-gpu-notes.md` (fork, mainline 6.18 and stock 3.0.2 compared);
reads on the fifteenth boot, always-on blocks only (`evidence-private/2026-11-05-y1-gpu-r1/baseline.txt`);
`mfgcfg` `0x13000000` deliberately not read while the MFG domain is off (a read could hang the bus).

- **Already ported:** the `mfgcfg` gate driver (`CLK_MFG_BG3D`, `0x13000000` bit 0), the topckgen MFG muxes
  and gates, the power-domain binding header, the TOPRGU MFG reset. **Missing:** the MT6572 data for mainline's
  `mtk-pm-domains` (the fork's `mt6572-pm-domains.h` plus a match entry), `DRM_LIMA` (6.18 accepts
  `"arm,mali-400"`; the Y2's patch 0038), and **`CONFIG_PM`**, off in every Y1 kernel so far, which the
  power-domain driver requires. Turning it on brings runtime PM to the eMMC (`mtk-sd`), USB (`musb`), UART and
  display drivers, a system-wide change.
- **MFG power sequence** (control `0x10006214`, status bit 4 of `0x1000660c`/`0x10006610`, SRAM bit 8, ack bit 12,
  no bus protection): fork/mainline and stock agree on the registers; stock adds short delays, releases SRAM
  before isolation and holds a display-domain clock while MFG is on. The SPM key is already enabled
  (`0x10006000` = 1), so no key write. Domain state: MFG `0xff12` (off), DIS `0x0d` (on), `PWR_STATUS` `0x3fed`.
  Stock makes MFG a child of DIS; the fork describes DIS as a sibling, which Linux would power **off** at late
  init if unclaimed (the display would go dark): the Y1 tree must disable the DIS child.
- **The GPU clock is unresolved.** Today `mfg_mux_sel` is on 26 MHz and the GPU path's PLLs are not clearly
  running: WHPLL is off (`0x10205240` `0xf0000010`, enable bit 0 clear), and UNIVPLL's enable bits read clear
  (`0x10205140` `0x30000000`) although Linux models it as a fixed 1248 MHz clock (USB works, so the reading is
  INFERRED as incomplete). Stock moves the GPU to **WHPLL 500.5 MHz** on this chip (eFuse `0x10009178` bit 27 = 0);
  the fork's own comment says without WHPLL "the GPU will get 0 Hz and hang". A GPU without a clock hangs the
  bus at Lima's first register read (watchdog reset).
- **`y2-gpu-check` refuses the Y1**: it requires two pixel processors (MP2); the MT6572 has one (MP1). A Y1
  check (one PP) is a small tool change.

**Revised R1** (needs the owner's admission, because it leaves §7's bounds):

| Stage | What | New beyond §7 |
| --- | --- | --- |
| R1a | `CONFIG_PM` alone, no GPU: a regression boot (eMMC root, USB networking, display, audio, charging, power-off) | runtime PM in existing drivers (their clock gating) |
| R1b | MFG domain (DIS child disabled), `mfgcfg`, Lima with no OPP table and no reset; the GPU clock from **WHPLL at stock's 500.5 MHz** | enabling WHPLL (a PLL, as stock does) |

## 10. R1a build — 2026-11-05

Owner-approved (R1a and R1b, the revised bounds of §9). `kernel/config/y1-sixteenth-boot.config`: the fifteenth
fragment plus `CONFIG_PM` (resolved: `PM_CLK`, `BLK_PM`; no suspend, no power domains), `LOCALVERSION` corrected;
`innioasis-y1-sixteenth-boot.dts`: the fifteenth tree, new model string; PID1 unchanged. Build
`out/y1-sixteenth-boot-1`, BOOTIMG sha256 `43d12137aa465cf6c201e9d034510f633432c022d59303b3170cbf9b66d9cea9`.
The fragment's old `# CONFIG_PM is not set` line was later dropped (the build warned about the override); the
resulting `.config` is unchanged.

## 11. R1b build — 2026-11-05

Owner-approved. `kernel/config/y1-seventeenth-boot.config`: the sixteenth fragment plus `MTK_SCPSYS_PM_DOMAINS`
and `DRM_LIMA` (resolved: generic power domains, `DRM_SCHED`, GEM shmem, `PM_DEVFREQ` and `PM_OPP`, unused
without an OPP table). Overlays: `0031-mt6572-pm-domains.patch` (the MT6572 match in `mtk-pm-domains.c`) with the
added `kernel/y1/mt6572-pm-domains.h`, which describes **only MFG** (so a DIS node can never be powered by
Linux), and `0032-lima-initial-runtime-idle.patch` (the Y2's 0038 byte for byte). Device tree
`innioasis-y1-seventeenth-boot.dts`: the DIS child disabled, the GPU node without `resets`, `assigned-clocks`
moving `mfg_mux_sel` to `mfg_pre_500m` (WHPLL, LK's divider: 500.5 MHz) and `mfg_sel` to the mux; no OPP table,
no `assigned-clock-rates`. The `mfgcfg` gate driver has no runtime PM, so the MFG domain stays on after probe:
this boot tests power-on only. Build `out/y1-seventeenth-boot-1`, BOOTIMG sha256
`99a426fa8cd99d0fb4ec67c433ccebadd9192521ac63705564cd657b1e4a1ef4` (`0x33a000` bytes), reproduced byte for byte.

Expected after boot (read-only checks): `PWR_STATUS` `0x3fed` → `0x3ffd` (bit 4 only); `MFG_PWR_CON` low byte `0x0d`
with SRAM bits 8 and 12 clear; `DIS_PWR_CON` still `0x0d`; `CLK_CFG_0` bits 10:8 = 1 and bit 31 = 1; WHPLL enabled;
Lima reports a Mali-400 with one PP. Test: the GPU identity through Lima's ioctl and a GLES render (a one-PP
build of the GPU check), then the display, audio, charging and SSH as in R1a.

## 12. R1b run — 2026-11-05

`evidence-private/2026-11-05-y1-gpu-r1b/run1.txt`; seventeenth boot, flashed by the owner (no readback file).
Lima: `gp - mali400 version major 1 minor 1`, `pp0` (one PP), `l2_cache0 32K`, bus and core rate 500500000,
`Initialized lima 1.1.0`; no new kernel warning. Read-back: `PWR_STATUS` `0x3ffd` (bit 4 only); `MFG_PWR_CON`
`0x0e0d` (on, SRAM bits 8 and 12 clear); `DIS_PWR_CON` `0x0d` unchanged; `CLK_CFG_0` `0x93001192` (`mfg_mux_sel`
= 1, `mfg_sel` = 1, nothing else changed); `CG_CTRL0` `0xffefff7b` (`MFG_MM` ungated, the 500 MHz pre-gate on);
WHPLL `0xf0000011` (enabled), `0x8109a000` unchanged (500.5 MHz); `mfgcfg` `0x13000000` = 1, read only with the
domain on. genpd: `mfg` on; the GPU runtime-suspended after probe (Lima's idle patch), `mfg_bg3d` gated while idle.
The image's `y2-gpu-check` read the identity through Lima's ioctl (`LIMA_GPU_ID=0x1` Mali-400,
`LIMA_PP_CORES=0x1`) and stopped, as written, on the Y2's two-core requirement; no render was attempted. The
build sandbox has no ARM C library or GL headers for a one-core variant, so **the first GLES render is Reborn's
own in R2**. **GPU power-up, clock and identity: PASS**; rendering open.

## 13. R2 — 2026-11-05

Owner-approved. **Power-on retest** (before R2, on the seventeenth boot with `CONFIG_PM`): after R1a's power-off
the owner's press did not wake the player (one long press, unsure; recovered with the reset button and
mtkclient); on the seventeenth boot it switched off and **a 5-second hold of the centre button** brought it back.
Kernel power management is not the cause; a firm hold of about 5 s is needed.

**Manual run on the seventeenth boot** (`evidence-private/2026-11-05-y1-reborn-r2/manual1.txt`): the R2 start
script by hand over SSH. `/data` an ext4 loop image on its own 80 MiB tmpfs, the RAM overlay's SSH files
copied in (SSH unaffected); the seeded `session.json` rejected as a saved session and its settings merged, as
R0 found. Reborn's log: `Mesa Project; EGL 1.5; Mali400; OpenGL ES 2.0 Mesa 26.2.4`, 480x360 XRGB8888, GBM linear
KMS; first frame 1.4 s after start; audio "unavailable" (no `Y2Audio`, as planned); RSS 26 MB. **The first GLES
render on the Y1: PASS** (the owner's photo: Reborn's first-run reports dialog on the panel). Without the input
renames the dialog cannot be answered (no Select).

**Build:** `kernel/platform/apt32f-wheel.c` (shared): with `innioasis,zone-input-name` the Y1 zones report on a
second input device of that name; without it (the Y2, earlier Y1 trees) nothing changes. Overlay
`0033-mtk-pmic-keys-label.patch`: an optional `label` names the PMIC key device (as `gpio-keys`). Device tree
`innioasis-y1-eighteenth-boot.dts`: zones `KEY_PLAYPAUSE KEY_RIGHT KEY_BACK KEY_LEFT` on "Y2 navigation buttons";
the PMIC keys labelled "Y2 navigation buttons" (the centre is Select; nothing is Power). Initramfs
`y1-eighteenth-boot.list`: `initramfs/y1-reborn` (`S90y1-reborn`, the service list with it last). Build
`out/y1-eighteenth-boot-1`, BOOTIMG sha256 `19938cfbdc0efceeb666c849da610679bea630cd01d192e343df87ad3a0dd348`
(`0x33a000` bytes). The wheel's frame test (`tests/test_dev_wheel.py`) errors before and after this change
(its harness predates the Y1 zone code: undeclared `u32`, `BIT`, `dev_err_probe`); open.

**Run 1** (`evidence-private/2026-11-05-y1-reborn-r2/run1.txt`): the input renames took effect (`Y2 navigation
buttons` twice, `APT32F click-wheel`), but Reborn did not start: the test PID1 installs only its listed
overrides into the RAM overlay, and `S90y1-reborn` was not on the list (rcS skips a missing script). Fix: the
script added to `initramfs/y1-tenth-boot.h`'s override list under `Y1_EIGHTEENTH_BOOT` (earlier PID1 binaries
unchanged), defined for the eighteenth variant. Rebuild `out/y1-eighteenth-boot-2`, BOOTIMG sha256
`f998171435c0a93862cd1929f3dc93b54d9f2bed3a468e1bc34392da2a33536a` (`0x33a000` bytes; `y1-init` `8590bdfa…`; kernel
`.config` and DTB as build 1). Meanwhile Reborn was started by hand on build 1 for the button test.
**Button test** (build 1, Reborn started by hand): the owner reports all buttons working: the wheel scrolls,
the centre selects (the first-run reports dialog answered), the top zone goes back, and play/pause and
next/previous respond. **Reborn's navigation on the Y1: PASS.** Open for R2: Reborn starting by itself on build 2.

## 14. R3 — sound in Reborn (plan, 2026-11-05)

The owner asked for sound after the R2 button test. Reborn's audio path (`crates/reborn-audio/native/audio.c`,
`baseline-220`, CONFIRMED): the card id `Y2Audio`; `/etc/y2linux/audio-enabled.json` must list the stream's format
and rate (the Y2's lists S16_LE at 44.1 and 48 kHz and falls back to 44.1 kHz, converting 48 kHz-family sources);
at stream open the simple mixer element **`Master`** is set by dB — **−24 dB** for headphones (−3 dB only for
the speaker, which Reborn uses only when a jack device reports the jack empty; the Y1 has no jack device, so
never) — and the **`Headphone`** element's playback switch is turned on; the user's volume is a software gain
in Reborn. The optional `HP Output Level` and `PCM Filter` controls are absent on the Y1 (Reborn then keeps −24 dB).

Changes:
- **Codec** (`kernel/y1/mt6323-codec.c`): the gain control renamed `Master Playback Volume` with a dB scale,
  step 7 = 0 dB, 2 dB a step (−14 dB at step 0; the steps are the stock HAL's, the absolute level is
  INFERRED). Reborn's −24 dB clamps to **step 0**, the quietest. Same register field, same write policy.
- **Card**: the simple card renamed `Y2Audio`; its headphone widget renamed `Headphone`, so the pin switch is
  `Headphone Switch` (checked on the device that the simple mixer treats it as the `Headphone` playback switch).
- **Speaker off**: `S90y1-reborn` turns the `Speaker` pin switch off before Reborn starts (the pin defaults on, and
  Reborn never touches it on the headphone path).
- **Profile**: a Y1 `/etc/y2linux/audio-enabled.json` (card `Y2Audio`, S16_LE, **44100 only**), installed by PID1.
- Test: amixer view of `Master`/`Headphone`/`Speaker`; play a test tone from Reborn with headphones held away from
  the ears first, then at the ear; the register read-back shows gain step 0; the speaker stays silent.

| Stage | Item | Bound |
| --- | --- | --- |
| R3 | **Sound in Reborn** | headphones only, the admitted audio registers and values; gain control exposed as `Master` (dB), started at step 0; speaker pin off; 44.1 kHz S16_LE |

**R3 build — 2026-11-05.** Owner-approved. `kernel/y1/mt6323-codec.c`: the gain control `Master Playback Volume`
with `DECLARE_TLV_DB_SCALE(-1400, 200, 0)` (manifest hash updated); device tree `innioasis-y1-nineteenth-boot.dts`:
card `Y2Audio`, widget and pin `Headphone`; `initramfs/y1-reborn/etc/y2linux/audio-enabled.json` (S16_LE, 44100)
installed by PID1 under `Y1_NINETEENTH_BOOT` (with its `/etc/y2linux` directory); `S90y1-reborn` switches the
`Speaker` pin off before Reborn starts. Build `out/y1-nineteenth-boot-1`, BOOTIMG sha256
`7baa013c52cbbb8d1c5bfef3cb3ccd6dacae275b884e9a6a548afca2173b9987` (`0x33a000` bytes).

**R3 run — PASS — 2026-11-05** (`evidence-private/2026-11-05-y1-reborn-r3/`). Nineteenth boot: Reborn started by
itself (**R2's open check: PASS**); the profile installed; the simple mixer shows `Master` (pvolume, dB),
`Headphone` (pswitch) and `Speaker` (pswitch, off). Two test tones (−30/−40 dBFS, 44.1 kHz) copied into `/data/music`
and found by Reborn's scan. The owner played both from Reborn: heard on the headphones as expected, nothing from
the speaker, no error. Reborn's log: S32_LE preferred, fallback to S16_LE at 44100 (`hw:CARD=Y2Audio,DEV=0`,
period 512, buffer 4096), `hardware_mixer_gain_db` −24.0, playback started and drained, PCM closed. Sampled during
playback: `AUDTOP_CON5` `0x0014` (gain fields 0: **step 0**), `AUDTOP_CON6` `0xf5ba` (on), `Master` −14 dB; after
the stream the stock power-down state (`0x37e2`) and Reborn's restore of `Master` to the earlier step 2; the
speaker pin stayed off. **Sound in Reborn (headphones): PASS.** Open: the speaker (needs jack detection), start/stop
pops, 48 kHz-family sources converted by Reborn (not yet heard).

## 15. Power off from Reborn — 2026-11-06

The owner asked for power-off from Reborn. Reborn (`baseline-220`) turns the player off from a Power hold
(`crates/reborn-platform/src/input.rs`: Power is `KEY_POWER` from `mtk-pmic-keys` only; a long press at 650 ms
starts `PowerHold`, `crates/reborn-ui/src/lib.rs` `POWER_HOLD` 1.2 s more → `Effect::PowerOff` through the power
daemon); its Restart/Power Off rows are on a "service" page no menu links to. The Y1 has no Power key and every
button already has a Reborn long press (Select's is the context menu). Design: the centre button (the PMIC power
key, Select) held **3 s** is also Reborn's Power: overlay `0033-mtk-pmic-keys-label-hold.patch` (replaces
`0033-…-label`; the label part unchanged) adds, with `innioasis,hold-power-ms`, a second input device named
`mtk-pmic-keys` reporting `KEY_POWER` from a delayed work 3 s after a press until the release (cancelled by an
earlier release). Total: about 4.9 s from the press to Reborn's turn-off; releasing earlier cancels. A release
between 3.0 and 3.65 s is a short Power press for Reborn (screen off, or the platform's refused sleep request).
Build `out/y1-thirtieth-boot-1` (the twenty-ninth plus the hold), BOOTIMG sha256
`c62483c23d56a78f84e79a5a9f220d7777ced76a430a1843f120dabcf2fb9932`.
**Run — PASS — 2026-11-06**: on battery, a centre hold of about 5 s in Reborn showed the turn-off ring and the player
powered off; it came back with the usual hold (owner: "works").
