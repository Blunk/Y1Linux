# Y1 root filesystem: the Y2's Buildroot userspace from the microSD

Status: **step 1 PASS** (2026-11-05, [run 2](#10-run-2--pass--2026-11-05)): the Y2's Buildroot userspace runs
on the Y1 from the eMMC's `ANDROID` partition ([§8](#8-redesign-the-card-is-built-in--2026-11-05): the card
is built in), read-only below a RAM overlay. Next: the Y2's init and services (review first), persistent
data. Follows the
[backlight plan](Y1-BACKLIGHT-PLAN.md) (PASS). The owner chose, 2026-11-05: the Y2's Buildroot userspace
(consistency with the Y2), step 1 an image file on the existing card, step 2 later an own ext4 partition
as on the Y2.

## 1. Goal

A real userspace on the Y1 (shell, coreutils, the Y2's tools) instead of the test PID1's built-in
commands, from the microSD, **without writing to any storage** in step 1.

Not in scope for step 1: the Y2's init and services (its platform daemon, player, charger and power
daemons stay off: they were written for Y2 hardware and need review first), persistent changes,
repartitioning, networking, audio.

## 2. Evidence

- **Card:** one partition `mmcblk1p1`, 116 GiB, **FAT32** (boot sector read read-only over the USB
  shell, 2026-11-05); the owner's data is on it.
- **Userspace:** building the Y2 root filesystem here needs about 15 GB free disk (11 GB free), the
  `Y2Reborn` checkout and Rust 1.99.0. The Y2 project publishes the same Buildroot image with every release:
  `baseline-220` (2026-11-04) has `rootfs.ext4.gz` (51 MB), with `SHA256SUMS` and a signed manifest. It is
  armv7 hard-float glibc; the Y2's production kernel config lists what it needs from the kernel.
- **Y1 kernel today:** `allnoconfig` plus each boot's lines: no VFP/NEON (hard-float userspace needs
  them), no `FUTEX`, `EPOLL`, `SIGNALFD`, `TIMERFD`, `EVENTFD`, `SHMEM`, no script execution, no
  filesystem driver at all.

## 3. Step 1 design: read-only image on the card

- **The image:** `rootfs.ext4` from the `baseline-220` release, verified against its `SHA256SUMS`,
  decompressed on the PC and copied by the owner onto the card as `y1/Y2ROOT.ext4` (card reader or the
  stock firmware's USB storage mode). The owner's files are untouched.
- **Kernel (ninth boot):** the eighth fragment plus `VFAT_FS` with code page 437 / ISO 8859-1, `EXT4_FS`,
  `BLK_DEV_LOOP`, `OVERLAY_FS`, `TMPFS`, `VFP`/`VFPv3`/`NEON`, `BINFMT_SCRIPT`, and the syscalls glibc and
  busybox need (`FUTEX`, `EPOLL`, `SIGNALFD`, `TIMERFD`, `EVENTFD`, `SHMEM`, `FILE_LOCKING`, `POSIX_TIMERS`
  …, taken from the Y2's `production.config`).
- **Mounting (test PID1):** `mmcblk1p1` read-only (`vfat ro`; the device is already `BLKROSET`), the
  image through a **read-only** loop device, ext4 `ro,noload` (no journal replay, so the image is never
  written), then an overlay with a **tmpfs** upper layer: the userspace sees a writable root, every change
  lives in RAM and is gone at reboot.
- **Running it:** the test PID1 stays PID1 (watchdog, telemetry, the USB shell). A new shell command
  `rootsh` starts the image's `/bin/sh` in a chroot on the USB serial port; leaving it returns to `y1#`.
  The Y2's `/sbin/init` is never started.

## 4. Step 2 (later, separate approval)

An ext4 partition `Y2ROOT` on the card (the owner repartitions after a backup, or a spare card), mounted
read-write, then the Y2's init with a reviewed, Y1-specific service set. Consistent with the Y2's layout.

## 5. Test

Owner copies the image; boot; over the shell: `rootsh`, then `uname -a`, `ls /`, `cat /etc/os-release`,
`busybox`, a few Y2 tools in `--help` mode, `mount` (everything read-only below the overlay), exit. The
card checksum region (first MiB) and the image's SHA-256 can be re-read afterwards to show nothing changed.

## 6. Admission (step 1)

| Item | Bound |
| --- | --- |
| **Mount the microSD's FAT32 partition read-only** | `ro` mount on an already read-only block device; no fsck, no write |
| **Run the Y2's userspace binaries on the Y1** | a chroot shell started by hand over USB; no Y2 init or service; changes only in RAM |
| **Download the Y2 release image** (`baseline-220` `rootfs.ext4.gz`, 51 MB) | public release, verified against its `SHA256SUMS`; kept in `out/`, not committed |

## 7. Step 1 build — 2026-11-05

- **Image:** `baseline-220` `rootfs.ext4.gz` and `manifest.json` downloaded to `out/y2-release-220/` and
  checked against the release's `SHA256SUMS` (`rootfs.ext4.gz` `4d49e386…c3d3f04e`); decompressed
  `rootfs.ext4` 512 MiB, sha256 `4ac13770add2392e2733d58fa398e2057cddcd1366bf1332fbc0f3d2e6e19472`, ext4
  label `Y2ROOT`, journal clean, features include `metadata_csum` and `orphan_file`; userspace armhf glibc
  (`/lib/ld-linux-armhf.so.3`), busybox, `/sbin/init`.
- **Kernel:** `y1-ninth-boot.config` = the eighth fragment plus `VFP`, `VFPv3`, `NEON`, `FUTEX`, `EPOLL`,
  `SIGNALFD`, `TIMERFD`, `EVENTFD`, `POSIX_TIMERS`, `SHMEM`, `TMPFS`, `ADVISE_SYSCALLS`, `BINFMT_SCRIPT`,
  `FILE_LOCKING`, `INOTIFY_USER`, `PROC_SYSCTL`, `UNIX98_PTYS`, `EXT4_FS`, `FAT_FS`, `VFAT_FS`, NLS 437 /
  ISO 8859-1, `BLK_DEV` + `BLK_DEV_LOOP`, `OVERLAY_FS`. Not `POSIX_MQUEUE` (needs `NET`, out of scope).
- **PID1** (`initramfs/y1-ninth-boot.h`): after the disks are set read-only, mounts `mmcblk1p1` `vfat ro`,
  attaches `y1/Y2ROOT.ext4` with `LOOP_CONFIGURE` and `LO_FLAGS_READ_ONLY`, mounts it `ext4 ro,noload`, an
  overlay with a 32 MiB tmpfs upper layer on `/newroot` with `proc`, `sysfs` and `/dev` (bind); `rootsh`
  forks a session leader on `ttyGS0` (descriptors above 2 closed), chroots and runs `/bin/sh -l`; PID1
  leaves the port alone until it exits.
- Tree `innioasis-y1-ninth-boot.dts`: the eighth tree, new model string.

Build `out/y1-ninth-boot-1`, BOOTIMG sha256
`d0db6378f7b81f120536ce0274ca5c3bdecfb6f0ed1ff9ee997280a53e82e6cd` (`0x253000` bytes). Physical status
NOT_RUN; the owner copies the image onto the card first.

## 8. Redesign: the card is built in — 2026-11-05

The Y1's microSD is mounted inside the player, so the owner cannot copy a file onto it. Owner's choice
(2026-11-05): replace stock Android, as the Y2's production layout does: `Y2ROOT` in the eMMC's `ANDROID`
partition.

- **Partition:** scatter `ANDROID` linear `0x4b40000`, size `0x28a00000` (650 MiB); physical (Linux
  `mmcblk0`) `0x4540000` by the measured mapping (logical − `0x600000`). Linux does not parse MediaTek's
  partition table, so PID1 reaches it through a loop device with that offset and size limit.
- **Restore path, ready before any write:** the 2026-11-04 backup holds stock's `ANDROID` intact (ext4
  magic, 166,400 blocks of 4 KiB = the whole partition); extracted as
  `evidence-private/2026-11-04-y1-backup/ANDROID-from-backup.img`, sha256
  `7b99f5cb274ef4836d9d63e923fdf4cf06db42af64a98a88c9e265eaa860f98b`. Writing it back restores stock
  Android exactly; with the stock BOOTIMG the player is then fully stock again.
- **The write:** the owner writes `rootfs.ext4` (512 MiB, `4ac13770…`) to `ANDROID` with mtkclient through
  the preloader (as BOOTIMG), then reads back the first 512 MiB and compares the hash. The partition's
  last 138 MiB keep stock bytes, which the 512 MiB ext4 filesystem never addresses.
- **Linux stays read-only:** loop `LO_FLAGS_READ_ONLY` with `lo_offset` `0x4540000` and `lo_sizelimit`
  `0x28a00000` on the `BLKROSET` `mmcblk0`, ext4 `ro,noload`, the tmpfs overlay, `rootsh`. The card and
  the FAT driver are no longer used (the kernel config keeps `VFAT` for step 2).
- **Consequences:** stock Android no longer starts (its BOOTIMG is already replaced by the test images);
  charging works under Linux (charging plan stage B). Recovery: BOOTIMG and ANDROID through the preloader.

Build `out/y1-ninth-boot-2`, BOOTIMG sha256
`589d0a767a682a128d52320b926cf71c47d2a2e93af1484e7d70784219d76b3c`.

| Admission (new) | Bound |
| --- | --- |
| **Write `Y2ROOT` into the eMMC `ANDROID` partition** (mtkclient, owner) | that partition only; the restore image and its hash ready first; read-back hash check; Linux itself still writes nothing |

The first admission item (mounting the card's FAT32) is no longer used.

## 9. Run 1 and correction — 2026-11-05

The owner wrote `rootfs.ext4` to `ANDROID` and the ninth BOOTIMG (`589d0a76…`); no read-back files were
kept. The Y1 itself shows the write is good: `loop0: detected capacity change from 0 to 1331200` (650 MiB),
`EXT4-fs (loop0): mounted filesystem 79324c69… ro without journal`, the overlay mounted (`/proc/mounts`;
`overlayfs: failed to set xattr on upper … falling back to uuid=null` is the tmpfs upper layer's limit).
`rootsh` started a child that ended at once: the session leader's exit hung up `ttyGS0` and PID1 reopened
it. Cause: `allnoconfig` leaves `ARM_THUMB` off, so the Y2's Thumb-2 glibc userspace cannot run; the Y2's
`production.config` also enables `KUSER_HELPERS` and `HIGH_RES_TIMERS`. Correction: those three in the
ninth fragment; PID1 now prints how the root shell ended (exit code or signal). Build `out/y1-ninth-boot-3`,
BOOTIMG sha256 `eb19644988f8c1edbfbe286bf9e6519fbeb2e2d20f13ea86d1426beeb89ad2d2`. Only BOOTIMG changes.

## 10. Run 2 — PASS — 2026-11-05

Build `eb196449…`; `evidence-private/2026-11-05-y1-rootfs/rootsh-2.txt`. `rootsh` gave the Y2's busybox
shell: `Linux (none) 6.18.56-y1-ninth-boot … armv7l GNU/Linux`; `/etc/os-release` Buildroot 2026.08
(`baseline-215-9-g31d3987e`), Y2Linux `1.0.0-beta.9.3.12`, rootfs `2026.08-platform-v1.243`; BusyBox
1.38.0; 234 programs in `/usr/bin`, the Reborn and Y2 services in `/usr/libexec` (none running: the only
processes were the shell and its children); 500 MB RAM, 23 MB used; root is the 32 MiB tmpfs overlay.

**Write verified on the device:** `head -c 536870912 /dev/loop0 | sha256sum` =
`4ac13770add2392e2733d58fa398e2057cddcd1366bf1332fbc0f3d2e6e19472`, the release image (59 s, about 9 MB/s,
through the read-only loop). `exit` returned to the PID1 shell. Linux wrote nothing to storage.

**Step 1: PASS.** The owner-approved eMMC write replaced stock Android's `ANDROID` partition with `Y2ROOT`;
the restore image is `ANDROID-from-backup.img` (§8).
