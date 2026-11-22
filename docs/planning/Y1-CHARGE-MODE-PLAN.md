# Y1 robust charging: a charge-only mode

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The step builds named
> below (the *N*th boot, its variant, tree, fragment and PID1 header) are the record of the test images that ran
> on the player; their files are folded into that build.

Status: **planned** (2026-11-06), not admitted. The owner asked to make charging robust before anything else.

## 1. The incident (2026-11-05/06)

A nearly flat Y1 on the twenty-first image (power daemon on) looped: boot → the daemon's voltage guard (3.4 V,
applied **even while charging**: `policy.rs` "USB presence does not prove positive net pack current") → power-off →
with USB attached a power-off is a restart ([power plan](Y1-POWER-PLAN.md) D1) → boot again. It never charged.
Earlier the same night the M1 image (configfs gadget, [MTP plan](Y1-MTP-PLAN.md)) had drained it while plugged in:
without a configured PC the Y1 charges at the 70 mA floor, below the running player's load. Recovered by charging
switched off from a power bank and flashing the nineteenth image (no daemon): 450 mA CC from the PC since.

## 2. Evidence

Agent notes `out/y1-charging-robust-notes.md` (read-only: the tree, the stock kernel, the Y1 LK disassembly; LK
function names INFERRED from MediaTek's reference LK). CONFIRMED unless marked.

- **The Y2 has a charge-only boot** (`tools/production/offline-charge.c`, run by its initramfs before the system):
  with a charger and either LK's charging boot mode or a battery under 3.4 V it charges with the system stopped;
  it leaves only on a deliberate power-key hold with three fresh samples ≥ 3.4 V; unplugging powers off. A
  critical shutdown on a charger becomes a "charger" watchdog reset, which LK turns into its charging boot mode,
  so the Y2 lands in charge-only mode, never back in the loop.
- **The Y1 has none of this.** Linux does not see LK's boot mode (the device tree is appended; LK's boot data at
  `0x80100100` is probably overwritten by the decompressing kernel, INFERRED). LK chooses its charging mode when a
  charger is present and the boot is not forced, and its low-battery limit is 3.25 V; LK itself does not charge.
- **No PMIC register latches the power-on cause**; the live signals are the charger-present bit, the power key and
  the watchdog status.
- **Wall chargers and power banks**: stock 3.0.2 detects the charger type (BC1.1: PHY bit at `0x1111081a`, PMIC
  `0x024`/`0x026`): host 500 mA, wall/non-standard 1000 mA, unknown 70 mA. The Y1 port skipped it (mainline MUSB
  lacks the Y2's USB coordination). The PMIC writes are already in the Y1 write policy.
- **A latched charger fault** ([power plan §12](Y1-POWER-PLAN.md)): once, the write policy refused a stop-sequence
  write (`-EPERM`) and the player did not charge until the cable was re-plugged; which register is not logged.
  A charge-only mode cannot help if that happens on a flat battery.

## 3. Stages

| Stage | What | Image |
| --- | --- | --- |
| **C0** | Read-only: watchdog status/mode after a key power-on, a charger power-on and a restart; whether LK's boot data survives; the quiet-system current. Logging only: the PMIC wrapper reports the register, value and old value of a refused write | one image (logging) |
| **C0b** | Fix the latched fault from C0's log (the likely cause: read-only detector bits changing between the driver's read and the policy's re-read) | with C1 |
| **C1** | **Charge-only mode in PID1**, before the Y2 system starts: with a charger and the battery under about 3.6 V the player stays in a small mode (no Reborn, no daemon, the backlight low, the voltage and charge state on screen, the USB gadget bound so a PC grants 450 mA); it boots the system on a centre-button hold of 2 s once three samples are at least 3.4 V; without a charger and under 3.4 V it powers off. With the daemon's critical shutdown on a charger then landing here on the next boot, the loop ends | yes |
| **C2** | Wall chargers and power banks: BC1.1 detection (stock's method) or, first, the rule "no USB host reset within N seconds of plug-in → not a PC → 450 mA" | yes |
| **C3** | The loop test: the daemon with a synthetic critical policy on a charger → charge-only mode, not a loop | test |
| **C4** | A full supervised charge: CV phase, 4.175 V termination, the timers (open since the charging plan) | test |

The configfs USB gadget (MTP M1) waits until C1 passes and its own failure is understood; every USB-related image
is tested with a well-charged battery from now on.

## 4. Risks

A charge-only mode that never exits (the centre-button hold, and the full system after reflashing). One that exits
too early (three fresh samples and a voltage floor). A source weaker than the quiet mode's load (the screen shows
the trend; the backlight stays low). BC1.1 coordination with the USB gadget (C2's interim rule first).

## 5. Admission

| Stage | Item | Bound |
| --- | --- | --- |
| C0 | **Diagnose** | reads only; a log line in the PMIC wrapper's refusal path |
| C1 | **Charge-only mode** | PID1 only; the existing charger driver and 450 mA ceiling; no new PMIC write |
| C2 | **Other chargers** | the interim rule first; BC1.1 (PHY bit, PMIC `0x024`/`0x026`) only with its own admission |
| C3–C4 | **Tests** | the owner present, on a charger |

## 6. C0 and C1 build — 2026-11-06

Owner-approved. On the twenty-first image (g_cdc, Y2DATA, the power daemon), not the paused twenty-second.

- **C0 log**: `kernel/patches-y1/0005-mt6572-pmic-wrap.patch` regenerated: every refusal of the Y1 write policy
  logs `write policy refused 0x<reg> = 0x<value> (now 0x<old>)` (rate-limited) before returning `-EPERM`. The
  watchdog registers and LK's boot data are read over SSH.
- **C1** (`initramfs/y1-twenty-third-boot.h`, `Y1_TWENTY_THIRD_BOOT`, called before the hand-over to busybox init):
  waits up to 15 s for the charger driver's first sample (`BAT0` `voltage_now`, `status`); none → the system
  starts. No charger: under 3.4 V power off, otherwise start. Charger and under 3.6 V, **or the centre button
  still held** (`EVIOCGKEY`): charge-only mode: backlight 4, a screen line every 10 s, the kernel's g_cdc gadget
  (so a PC grants 450 mA); a release after a hold of at least 2 s starts the system if the last three 1 s samples
  were at least 3.4 V (otherwise the screen says so); three samples without a charger → power off. Only reads and
  the backlight.
- Build `out/y1-twenty-third-boot-1`, BOOTIMG sha256
  `2889dbdd7be956ca7abe6126ae4efc2a6700620a22477b3495ef82a71b588fe5` (`0x33b800` bytes).

## 7. C0/C1 run 1 — 2026-11-06

`evidence-private/2026-11-06-y1-charge-c1/`. Build 1, battery 69 % (3.87 V), on the PC.

- **Normal boot**: no charge-only mode (above 3.6 V, button up); Y2DATA, Reborn, the daemon; 450 mA CC; no
  write-policy refusal logged.
- **C0**: after this boot (it followed an mtkclient flash) the watchdog mode reads `0x59`, status `0` (`0x1000700c`),
  `0x10007018` `0`. At `0x80100100`, where LK leaves its boot data, there is ARM code (`0xe34c5070 0xe1a00005`):
  **the kernel overwrites LK's boot data** (the research's inference, confirmed); Linux cannot learn LK's boot mode
  that way.
- **The held-button entry failed twice** (owner held the centre button through the boot): `mtk-pmic-keys` reports
  only changes, and a button already down before it probed never sends a press, so `EVIOCGKEY` reads it as up.
  Fixed: `centre_held()` reads the PMIC's own level first, CHRSTATUS `0x142` bit 1 (PWRKEY_DEB, clear while pressed,
  as `poweroff-policy.h`). Confirmed on the Y1: three presses over SSH read `0x0054` while held and `0x0056`
  released, matching the key driver's events. Build 2: `out/y1-twenty-third-boot-2`, BOOTIMG sha256
  `6e3f759c985526291dadaa02c810c8ea86f41f8b5b7467dc17c93eb3eed67516` (`0x33b800` bytes).

## 8. C1 run 2 — 2026-11-06

Build 2 (`6e3f759c…`), on the PC, the owner holding the centre button through the boot: **charge-only mode
entered** (screen dimmed, the mode's line shown, no Reborn); `usb0` present on the PC without an address on the Y1
(the system's network not started, as designed). The charger log during the mode (`run2.txt`): at 3.8 s and 63 s
`status=1 fault=0x0 … source=1 limit=450000uA` (battery 3.78 → 3.89 V under charge): **450 mA from the PC in
charge-only mode**. The owner's 2 s hold started the system (Y2DATA mounted at 113 s); Reborn ran, charging went on;
no write-policy refusal. **Entry by the held button, charging and the exit: PASS.** Open: unplug in the mode
(power-off), the automatic entry under 3.6 V (waits for a naturally low battery), C3 (the daemon's critical shutdown
on a charger lands in the mode).

**Unplug test** (build 2, the same day): charge-only mode entered by the held button, USB unplugged: the player
**powered off**; switched on again with the centre button and plugged in, it started normally (`run3.txt`).

## 9. The Y2's charging screen — 2026-11-06

The owner asked for the Y2's charging screen instead of text. The Y2's `tools/production/offline-charge.c` runs from
its initramfs and is not on the Y1's rootfs; the Y1's PID1 is freestanding (no libc, no soft-float). Ported into
the charge-only mode (`initramfs/y1-twenty-ninth-boot.h`, `Y1_TWENTY_NINTH_BOOT`): the same artwork rendered by
Reborn (`tools/graphics/offline-charge-art.h`), layout, colours, title (Charging / Charged / Charging paused) and
hint (Hold Power to turn on / Release to start / Battery too low to start) logic, the ring sweeping at 2 frames a
second while charging and full when charged, the screen dark (backlight 0, `FBIOBLANK` power-down) 8 s after the
last press and lit by a press, the console in `KD_GRAPHICS` while it owns `/dev/fb0` (DRM, XRGB8888 480x360,
checked at open; text mode otherwise). The Y1's own logic stays: the centre button (the Y1's power key), the 2 s
hold, three samples at 3.4 V, power-off on unplug. The ring's coverage uses integer sixths of a pixel and 30-degree
sectors instead of floats: compared with the Y2's float code on the PC over all 12 positions, 4 pixels differ, by
at most 19/255 in one channel (samples on a sector edge). Build `out/y1-twenty-ninth-boot-1`, BOOTIMG sha256
`03784841d957d6a553b983eaf5161baf13c4a5327f900585baa10b8a47869ae4`; the twenty-eighth's PID1 recompiled byte-identical.
The hint says "Power": on the Y1 the centre button is the PMIC power key.

**Run — PASS — 2026-11-06** (twenty-ninth boot, on the PC, entered by holding the centre button): the owner sees the
Y2's charging screen in the charge-only mode ("works, shows nice image now while charging").
