# Y1 system: the Y2's init and a Y1-safe service set

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The step builds named
> below (the *N*th boot, its variant, tree, fragment and PID1 header) are the record of the test images that ran
> on the player; their files are folded into that build.

Status: **milestone A PASS** on the owner's player (2026-11-05, [run 3](#10-run-3--milestone-a-pass--2026-11-05)):
the Y1 boots into the Y2's own init with a Y1-safe service set, nothing written to storage. Next milestones
(data, USB networking, power, player) each need their own admission.
Follows the [root filesystem plan](Y1-ROOTFS-PLAN.md) (step 1 PASS: the Y2's userspace runs on the Y1 in
a chroot shell, read-only below a RAM overlay).

## 1. Goal

The Y1 boots into the Y2's own system: busybox init as PID 1, the Y2's boot scripts, a login shell on the
USB serial port, with only services that are safe on Y1 hardware. Milestone A still writes nothing to
storage; later milestones add a data partition, USB networking with SSH, and the player.

## 2. Evidence: the Y2's services on the Y1

Review of the `baseline-220` image (`/etc/inittab`, `/usr/libexec/reborn-boot-services`, `/etc/init.d/*`)
and the `y2-platform` sources in this repository (`platform/crates/y2-platform/src/commands/`), 2026-11-05.
Nothing in a normal Y2 boot writes PMIC, charger or clock registers, loads modules or touches `/dev/mem`,
and nothing opens `/dev/watchdog` in steady state. Most risky paths are gated on a verified `Y2DATA`
(`/data/.y2data-schema` and `/run/y2-data-device`), which the Y1 does not have.

| Group | Scripts | On the Y1 |
| --- | --- | --- |
| Safe | inittab sysinit lines, `S00y2-runtime`, `S02klogd`, `S02sysctl`, `S11modules` (no list), `S25y2-display` (no-op), `S50crond` (no crontab) | work as they are |
| Safe with infrastructure | `S03y2-time` (needs `/run/y2`; build-date clock, no RTC), `S01syslogd` (needs `UNIX` sockets; logs to `/data`, so redirected to RAM) | after the milestone A changes |
| Later (networking, data) | `S30dbus-daemon`, `S40network`, `S20y2-usb` (waits for `usb0`), `S50dropbear` (keys and schema on `/data`), `S43y2-time` (NTP), `S02y2-data`, `S03seedrng`, `S03y2-platform`, boot-stage/boot-evidence | need `Y2DATA`, networking, a configfs gadget |
| **Off on the Y1** | `S01y2-platform` (Y2 diagnostics, `.ko` loader, would take over `ttyGS0`), **`S02y2-media` (can `mkfs.ext4` the stock FAT partition; inert only because Linux does not parse the Y1's partitions)**, **`S04y2-power` (low-battery and timed `poweroff`: the Y1 has no power-off path, so it would only halt and keep draining; Y2 battery profile)**, `S05y2-cpu-idle` (Y2 SPM), `S41y2-connectivity` (MT6582 radio), `S40bluetoothd`, **`S42y2-readiness` (replaces the USB gadget with MTP, mounts the SD read-write, can run `y2-update-core`, a raw MISC write on `mmcblk0`)**, `S90reborn` (needs data, audio, power daemon; timed poweroff), `reborn-splash` (hides the console without sockets) | disabled |

## 3. Milestone A design: Y2 init, nothing written

- **Kernel (tenth boot):** the ninth fragment plus `NET` and `UNIX` (local sockets only; no network device
  but `lo`, no INET yet).
- **Y1 overlay files**, in the initramfs and copied by PID1 into the RAM overlay's upper layer before it is
  mounted (the image stays untouched): `/etc/inittab` (the Y2's, plus
  `ttyGS0::respawn:/sbin/getty -n -l /bin/sh 0 ttyGS0`), `/etc/init.d/rcS` (runs only the safe list above,
  logs each script), `/usr/libexec/reborn-boot-services` (without splash, boot evidence and boot stages),
  `/etc/default/syslogd` (log ring in RAM, `-C`), `/run/y2` created at boot.
- **PID1** prepares as in the ninth boot (disks `BLKROSET`, `Y2ROOT` read-only loop, overlay), then
  `switch_root`s: `mount --move` the new root onto `/`, `chroot`, `exec /sbin/init`. The Y1 telemetry and
  the `y1#` shell end there; the Y2 system's shell takes over `ttyGS0`.
- **Watchdog:** in this variant PID1 never opens `/dev/watchdog`, so the kernel's boot handler
  (`WATCHDOG_HANDLE_BOOT_ENABLED`) keeps feeding it, as on the Y2. A hang is then no longer reset by the
  watchdog (the Y2's behaviour); a stuck player is recovered through the preloader as before.
- **Not available yet:** power-off (`poweroff` halts the CPU; the charger's hardware watchdog still stops
  charging within 4 s), the clock (build date), persistent settings and logs.

## 4. Later milestones (each with its own admission)

- **B, data:** a `Y2DATA` ext4 for settings, logs, SSH keys (the Y2 uses stock `USRDATA`; on the Y1 the
  2 GiB `USRDATA` is the candidate), with the Y2's schema and the verified-device record.
- **C, USB networking and SSH:** configfs and libcomposite with ECM (or ACM + ECM), `INET`, the Y2's
  `10.42.0.1` link, dropbear.
- **D, power:** a Y1 power-off path (MT6323 / RGU), a Y1 battery profile, then the power daemon.
- **E, the player:** audio (AW87559 amplifier, MT6572 audio), input, suspend, then Reborn.

## 5. Test

Boot; the screen shows the Y2's boot messages; on the PC `ttyACM0` gives the Y2 system's shell. Check
`ps` (busybox init, klogd, syslogd, crond, the getty), `logread`, `mount` (root still the read-only image
below the RAM overlay), `cat /proc/mounts`, `date`, the backlight and charger sysfs; `reboot` restarts.

## 6. Admission (milestone A)

| Item | Bound |
| --- | --- |
| **Run the Y2's init as PID 1** with the Y1 service list | only the safe scripts of §2; the overrides live in the RAM overlay; the image is unchanged |
| **Kernel networking core for local sockets** (`NET`, `UNIX`) | no network interface but loopback, no INET |
| **Leave the watchdog to the kernel's boot handler** | as on the Y2: no userspace feeding, so no reset on a userspace hang |

## 7. Milestone A build — 2026-11-05

- **Overrides** (`initramfs/y1-system/`, in the initramfs by `initramfs/y1-tenth-boot.list`): `etc/inittab`
  (the Y2's lines plus `ttyGS0::respawn:/sbin/getty -n -l /bin/sh -L 0 ttyGS0 vt100`), `etc/init.d/y1-services`
  (`S00y2-runtime S01syslogd S02klogd S02sysctl S03y2-time S11modules S25y2-display S50crond`),
  `etc/init.d/rcS` and `rcK` (run or stop only that list; rcS keeps the boot trace),
  `etc/default/syslogd` (`-C256`, a RAM ring), `usr/libexec/reborn-boot-services` (creates `/run/y2`, runs
  rcS; no splash, boot evidence or boot stages).
- **PID1** (`initramfs/y1-tenth-boot.h`): the ninth boot's preparation with the overrides copied into the
  upper layer before the overlay is mounted; then descriptors above stdio closed, the new root moved onto
  `/` (`MS_MOVE`), `chroot`, `exec /sbin/init`. `/dev/watchdog` is never opened in this variant (the kernel's
  boot handler feeds it); on any failure PID1 keeps its own session and `y1#` shell.
- **Kernel** `y1-tenth-boot.config`: the ninth fragment plus `NET`, `UNIX` (`INET` and wireless off).
  **Tree** `innioasis-y1-tenth-boot.dts`: the ninth tree, new model string.

Build `out/y1-tenth-boot-1`, BOOTIMG sha256
`fc2b360477a47e50af7da82cf7a727ad3af196172144ceb2ee0e935f0855dd8a` (`0x29f000` bytes). Only BOOTIMG changes;
`Y2ROOT` stays as written.

## 8. Run 1 and correction — 2026-11-05

Run 1 (`fc2b3604…`; `evidence-private/2026-11-05-y1-system/run1-*.txt`): the hand-over worked: PID 1 is
the Y2's busybox `init` on `6.18.56-y1-tenth-boot`, the getty shell answers on `ttyGS0`, the boot trace
lists exactly the eight allowed scripts, root is the overlay, the Y2 fstab mounted `/run`, `/tmp`,
`/dev/pts`, `/dev/shm`; under it the kernel keeps charging (`BAT0` Charging), the backlight (32) and the
wheel. Running: `init`, `klogd`, `crond`, the shell; nothing outside the list.

Faults:
- **`sleep 1` returned after 0.01 s**, so `init` (25 s of CPU in 65 s) and `crond` spun. ~~The ninth
  boot's `HIGH_RES_TIMERS` … puts the clock events on the MT6572 GPT's one-shot path, which fires a tick
  early.~~ **Wrong diagnosis, corrected in §9.** `HIGH_RES_TIMERS` was taken off in run 2 and stays off.
- **syslogd did not start:** this busybox has no `logread` ring (`-C`). Correction: `-O /run/y2/system.log
  -s 256 -b 1` (the Y2's rotation, in RAM).
- The shell's `read` returned nothing from `/proc/uptime` (empty boot-trace times); to be rechecked with
  correct timers.

Build `out/y1-tenth-boot-2`, BOOTIMG sha256
`83f27111c5bb0bc83e9f85d7d35fd86da050ea9e08c30bc03883b0475c67afd0`.

## 9. Run 2 and correction — 2026-11-05

Run 2 (`83f27111…`): syslogd runs (`/run/y2/system.log` with the boot messages and init's getty start).
`sleep 1` still returned at once with exit 0, the wall clock did not advance across `sleep 2`, init still
spun. `strace` in the image: `clock_nanosleep(CLOCK_REALTIME, 0, {tv_sec=1, …}) = -1 ENOSYS` and the
shell's `read` from `/proc/uptime` `ppoll(…) = -1 ENOSYS`. The Y2's glibc userspace uses the 32-bit-time
syscalls, which `allnoconfig` leaves out (`COMPAT_32BIT_TIME`, line 9 of the Y2's `production.config`, missed
by the ninth boot's option extraction); busybox ignores the error. The test PID1 only ever used the
64-bit-time calls, so earlier boots could not show it. The timer was never at fault.

Correction: `COMPAT_32BIT_TIME` in the tenth fragment. Build `out/y1-tenth-boot-3`, BOOTIMG sha256
`bdf7f0632c32ce35589f5e4f098d2197395e10faeade0d55e7dd1d0f4bea3eb6`.

## 10. Run 3 — milestone A PASS — 2026-11-05

Run 3 (`bdf7f063…`; `evidence-private/2026-11-05-y1-system/run3-a.txt`): `time sleep 1` = 1.00 s; the
shell's `read` from `/proc/uptime` works and the boot trace carries times (each script about 20 ms,
13.95–14.2 s after boot); processes: busybox `init` (1 s of CPU, at boot), `syslogd`, `klogd`, `crond`,
the getty shell, kernel threads; load 0.19, 40.8 of 43.6 s idle; the clock at the build floor
(`/run/y2/time.json` `"source":"build_floor"`, no RTC). Root is the read-only `Y2ROOT` below the RAM
overlay; nothing outside the eight allowed scripts runs; the kernel keeps charging, display, backlight and
the wheel.

**Milestone A: PASS.** Corrections on the way: syslog to a RAM file (no `-C` ring in this busybox),
`COMPAT_32BIT_TIME` (the Y2's glibc uses the 32-bit-time syscalls; run 1's `HIGH_RES_TIMERS` diagnosis was
wrong, the option stays off).
