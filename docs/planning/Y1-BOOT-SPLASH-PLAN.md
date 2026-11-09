# Y1 quiet boot with the Reborn splash

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The step builds named
> below (the *N*th boot, its variant, tree, fragment and PID1 header) are the record of the test images that ran
> on the player; their files are folded into that build.

Status: **PASS** (2026-11-06, [run](#3-run--pass--2026-11-06)). The owner asked for the Y2's boot screen instead of kernel text. No new
hardware scope: the kernel console level, PID1's output and a userspace program already on the rootfs.

## 1. Evidence

- **The Y2** (`kernel/dts/innioasis-y2.dts` bootargs `loglevel=3`; `initramfs/production/init` starts
  `/sbin/reborn-splash` early; the rootfs `reborn-boot-services` runs `reborn-splash --start` before `rcS`):
  `tools/graphics/reborn-splash.c` is an early KMS owner with one CPU-mapped dumb buffer (no GPU), the Reborn
  wordmark, a progress bar driven by milestones in `/run/reborn-splash/phase`, and a hand-over to Reborn when it is
  ready. It is drawn for 480x360, the Y1's panel too, and is on the Y1's rootfs (`/usr/libexec/reborn-splash`).
- **The Y1 today**: `loglevel=8 ignore_loglevel` (every kernel line on the screen); PID1 prints its progress to the
  console; its development diagnostics still run on every boot (disk digests, the kernel's USB log on screen and
  the seventh boot's ten-second colour bars, `BAR_SECONDS`): PID1 starts at 1.3 s, the Y2 system at about 14 s. The
  Y1's `reborn-boot-services` override left the splash out (system milestone A wanted the console visible).

## 2. Design (twenty-eighth boot)

- Bootargs `loglevel=3` (the Y2's) and `vt.global_cursor_default=0` (no cursor on the black console before the
  splash); the serial console keeps everything.
- PID1 (`initramfs/y1-twenty-eighth-boot.h`, `Y1_TWENTY_EIGHTH_BOOT`): its standard output and error go to
  `/dev/ttyS0` from the first line (a static node in the initramfs list); the diagnostics are skipped (only the
  wait for the eMMC remains); the screen gets PID1's text back for the charge-only mode (including its power-offs)
  and when the Y2 system does not start.
- `initramfs/y1-splash/usr/libexec/reborn-boot-services`: as the Y2's, `reborn-splash --start` before `rcS`
  (without the Y2's boot evidence and boot stages).
- Expected: the Innioasis logo (LK), a short dark screen, the Reborn splash, Reborn. The Y2 starts its splash even
  earlier from its initramfs; on the Y1 PID1 does not share `/run` with the system, so the splash starts at the
  Y2's own hand-off point.

Build `out/y1-twenty-eighth-boot-1` (on the twenty-seventh: MTP fix, MTP at boot), BOOTIMG sha256
`8b76668ae94a157a4e22a900d6f68e34d2084ef68d3d964636540639a21ded58`; the twenty-seventh's PID1 recompiled byte-identical
after marking the skipped diagnostics `unused`.

## 3. Run — PASS — 2026-11-06

The owner: the boot shows the Innioasis logo, then the Reborn splash, then Reborn ("nice, works"). Timeline
(kernel time): PID1 1.2 s, gadget 1.6 s, the Y2 system's `rcS` 2.06 s (was 14.4 s), splash `console_hidden` 2.05 s
and `visible` 2.4 s, Y2DATA 2.25 s, MTP 3.2 s; Reborn's phases reach the splash (`model_restored` 3.5 s,
`graphics_ready` 8.5 s) and Reborn takes the display (explicit KMS presentation). The splash's log is
`/data/reborn/logs/splash-boot.jsonl`.
