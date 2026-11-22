# Y1 recovery image and over-the-air updates

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The design follows the
> Y2's [recovery and updates](../RECOVERY-AND-UPDATES.md) and [platform OTA v2](../architecture/platform-ota-v2.md).

Status: **R0 done, R1 PASS, R2, R3 built, R4, R4b and R5 PASS on the device, R6 done** (2026-11-13: `ota` on for
the Y1, the docs and the release procedure, [§13](#13-r6--release--2026-11-13); 2026-11-12, [R0](#6-r0--lks-recovery-door--2026-11-12),
[R1](#7-r1--the-rtc-door-on-the-y1--pass--2026-11-12), [R2](#8-r2--the-guard-by-mode--2026-11-12),
[R3](#9-r3--the-y1s-updater-and-packages--2026-11-12), [R4](#10-r4--pid1s-recovery-and-rescue--2026-11-12),
[R4b](#11-r4b--the-normal-systems-update-service--2026-11-12)): the RTC
PDN1 recovery flag starts RECOVERY on the Y1 by a Linux restart, a centre-button start and a charger start, and LK
clears it each time; each image's tree carries its own eMMC write windows; `y1-update-core` installs Y1 packages
with the Y1's geometry and sets the flag with every request; both images carry it in their initramfs and PID1 runs
it as the recovery and as the BOOTIMG rescue, host-built and selftested; the normal system runs the update
service's daily check and health acknowledgement with `y1-update-core` and reports OTA off (host code and tests).
R4's device run passed: the probe from the normal system went through the recovery and back, the guard refused
BOOTIMG and ANDROID from the normal system, and R4b's service runs with every health check an update needs OK.
R5 passed ([§12](#12-r5--install-cancel-failed-health-go-back--2026-11-12)): install, cancel, failed health with
rollback and go back, each restarting by itself. R6 enabled `ota` for the Y1 and wrote the user's side
([INSTALL](../INSTALL.md#updating), [Recovery and updates](../RECOVERY-AND-UPDATES.md#on-the-y1)); what is left is the
owner's: building, signing and publishing the first Y1 release with an update ([§13](#13-r6--release--2026-11-13)).
The release channel (GitHub Releases of Blunk/Y1Linux) and the Y1 release key (`y1linux-release-20261112`,
`tools/update/trust.json`) are in place since `ea684047`. Before this plan the Y1 was installed and updated only
with mtkclient. The goal is the Y2's chain on the Y1: a Y1 recovery image in RECOVERY, written once with mtkclient,
that installs signed updates (BOOTIMG, Y2ROOT, RECOVERY) and restores the previous version if anything fails.

This widens the Y1's write scope from USRDATA alone to BOOTIMG, ANDROID, RECOVERY and MISC. The
[roadmap/gap audit](roadmap-gap-audit.md#y1-recovery-r2-guard-by-mode-entry-audit--2026-11-12) was repeated with
R0/R1's hardware evidence before R2.

## 1. Evidence

Offline; CONFIRMED unless marked.

- **The partitions** (`evidence-private/2026-11-04-y1-rom-3.0.2/rom/MT6572_Android_scatter.txt`; Linux's address =
  scatter − `0x600000`, [data plan](Y1-DATA-PLAN.md#1-evidence)). Linux sees `mmcblk0` (7569408 sectors) without
  partitions, so everything below is reached by offset:

  | Partition | Scatter | Linux (bytes) | Linux (sectors) | Size | Today |
  | --- | --- | --- | --- | --- | --- |
  | BOOTIMG | `0x3180000` | `0x2b80000` | `0x15c00` | 6 MiB (`0x3000`) | the Y1 kernel + PID1 |
  | RECOVERY | `0x3780000` | `0x3180000` | `0x18c00` | 6 MiB (`0x3000`) | stock Android recovery |
  | MISC | `0x3dc0000` | `0x37c0000` | `0x1be00` | 512 KiB (`0x400`) | stock |
  | ANDROID | `0x4b40000` | `0x4540000` | `0x22a00` | 650 MiB (`0x145000`) | Y2ROOT (512 MiB), read-only loop |
  | USRDATA | `0x4d540000` | `0x4cf40000` | `0x267a00` | 2 GiB (`0x400000`) | Y2DATA, the only write window |

- **Stock LK knows a recovery door** (`strings` of the 3.0.2 `lk.bin`): `mboot_recovery_load_misc`, `boot-recovery`,
  `Check recovery boot`, `Enable recovery mode`, `[Recovery    Mode]`, `Detect key`, `RTC_PDN1_FAC_RESET`,
  `mboot_android_load_recoveryimg_hdr`, `load recovery.img`. R0 traced the code
  ([§6](#6-r0--lks-recovery-door--2026-11-12)): LK reads MISC, a key and the RTC flag, but only the RTC flag survives
  its power-off charging check; LK clears that flag. Not yet seen on the device (R1).
- **The stock kernel enters recovery through the RTC**, not MISC: `arch_reset("recovery")` sets RTC PDN1 bits 5:4 to
  `0x10` (`rtc_mark_recovery`, `out/y1-power-notes.md`). So the Y1 LK checks at least PDN1. The PMIC write policy
  refuses RTC writes from Linux today.
- **The Y1 BOOTIMG** (`out/y1-boot-1.0.0-beta.9.3.24/BOOTIMG.img`): 4,179,968 bytes, `ANDROID!` header named
  `Y2Linux-M1`, MTK `KERNEL` header (`0x58881688`): the same format and **the same name as the Y2's**. 2.1 MB are
  left in a 6 MiB partition. The initramfs is built into the kernel; the separate ramdisk is empty.
- **The kernel write guard** (`kernel/y1/emmc-guard.h`, overlay 0034): one window from
  `innioasis,emmc-write-window`, USRDATA. Every other write is refused.
- **What is Y2-only today**:
  - `tools/update/core.c`: `DISK_SECTORS 15203328`, `IMAGE_BYTES` 16 MiB, `BOOTIMG_OFFSET`, `RECOVERY_OFFSET`,
    `MISC_OFFSET`, the disk lookup by size in `disk_device()`, the root target (a Y2ROOT partition).
  - `initramfs/production/recovery.sh` and the BOOTIMG rescue (`boot_check()`) run from the Y2's shell init and find
    partitions by name (`y2_find_partition`); the Y1 boots `initramfs/y1-init.c`.
  - `tools/update/package.py`: `boot_name` checks `Y2Linux-M1` / `Y2Recovery-1`, the recovery limit is 16 MiB.
  - `platform/crates/y2-platform/src/commands/service/update.rs`: finds `/data` as an eMMC partition
    (`mmcblk0p9`); on the Y1 `/data` is a loop over USRDATA.
  - `S42y2-readiness` (runs `y2-update-core`, a raw MISC write) is off on the Y1
    ([system plan](Y1-SYSTEM-PLAN.md)); `platform/capabilities.json` reports `ota` enabled for both players.
  - `tools/update/recovery-ui.c` draws for the Y2's display; the Y1's is 480×360.

## 2. Design

- **Same chain as the Y2.** The normal system downloads and checks the signed package, writes the new recovery image
  into RECOVERY, reads it back, sets the request and restarts. The recovery verifies again, backs up BOOTIMG and
  Y2ROOT to Y2DATA, writes and reads back both, records *pending health*, clears the request and restarts. The new
  system confirms its health or the next start restores the previous version.
- **The door** (after R0): the **RTC PDN1 recovery flag** (bits 5:4 = `01`, as stock's `rtc_mark_recovery`). LK
  clears it at every start, so the recovery **re-arms it as soon as it runs** and keeps it set until its work is
  verified; the last step clears flag and MISC together. MISC `boot-recovery` stays as the persistent record the
  BOOTIMG rescue reads (`boot_check()`: "request set, recovery did not start"), but on its own it does not start the
  recovery on the Y1. Linux needs a PMIC write-policy exception for PDN1 bits 5:4 and the RTC write trigger
  (`WRTGR` `0x803c`), nothing else in the RTC.
- **One kernel, two device trees**, as on the Y2 (`Y2_RECOVERY`): `innioasis-y1.dts` with `-DY1_RECOVERY` adds
  `y2.recovery=1` to the boot arguments and sets the recovery write windows. Nothing else differs; a check like
  `tools/validation/dev_artifacts.py:recovery_image` proves that.
- **The guard by mode**: `innioasis,emmc-write-window` becomes a list of windows, so each tree carries its own
  policy:

  | Booted from | May write (sectors) |
  | --- | --- |
  | BOOTIMG (normal) | USRDATA `0x267a00+0x400000`, RECOVERY `0x18c00+0x3000`, MISC `0x1be00+0x400` |
  | RECOVERY | USRDATA, BOOTIMG `0x15c00+0x3000`, ANDROID `0x22a00+0x145000`, MISC |

  Neither side writes the image it runs from; the preloader, LK, MBR/EBR1, NVRAM, PROTECT_F/S, SECCFG and the
  boot/RPMB partitions stay unwritable in both.
- **Per-device geometry in `core.c`**: a Y1 table (the offsets above, 6 MiB images, 7569408 sectors, the root
  target ANDROID with its 512 MiB image) chosen at build time, not at runtime. The disk is found by the Y1's size
  and controller (`11120000.mmc`).
- **Packages name their device.** Today a Y2 BOOTIMG passes the Y1's name check. The Y1 boot image is named
  `Y1Linux-M1` and its recovery `Y1Recovery-1`, and the signed manifest carries `"device": "y1"`; `core.c` and
  `package.py` refuse the other player's images. The separate trust key already keeps Y2 releases off the Y1.
- **PID1** (`y1-init.c`, a new `y1-update.h`): at a normal start, the BOOTIMG rescue step (`y2-update-core boot`:
  resume, roll back, clear a stale request) after Y2DATA is attached and before `switch_root`. With `y2.recovery=1`:
  attach Y2DATA, run the installer and `y2-recovery-ui`, feed the watchdog only while the installer makes progress
  (Beta 8.23), keep the previous start's record, restart or power off as the Y2 does. Never start the normal system
  from the recovery, never format.
- **Userspace**: a Y1 `S42` that runs only the update parts of `S42y2-readiness` (no USB gadget or SD changes: the Y1
  keeps its MTP gadget); the update service finds `/data` by the Y2DATA label and UUID, not the partition number;
  the power daemon answers the "charger or half a battery" check; the health check accepts the Y1's subsystems;
  a Y1 `capabilities.json` that reports `ota` only once R5 passes.
- **Size budget**: RECOVERY.img ≤ `0x600000` bytes. The kernel with the Y1 initramfs is 4.18 MB; the installer
  (`y2-update-core`, `y2-recovery-ui`, zlib) has to fit in the rest. Measured in R3 and R4: both images 5.66 MB with
  the installer (618 KiB left), without e2fsck ([§10](#10-r4--pid1s-recovery-and-rescue--2026-11-12)).

## 3. Stages

| Stage | What | Writes |
| --- | --- | --- |
| **R0** | Offline: disassemble the 3.0.2 LK's boot-mode decision (MISC, RTC PDN1, keys; what it clears, what it does with a bad image); back up RECOVERY and MISC from the existing dump; read both on the Y1 and compare | none to the Y1 (**done**) |
| **R1** | Probe image: the current kernel with a recovery tree and a PID1 that only logs `boot_mode`, shows *Recovery works* and powers off. Written to RECOVERY with mtkclient, read back. Then, on the UART: (a) the PDN1 flag set from a probe BOOTIMG build (one RTC write, the stock sequence) → LK starts RECOVERY, by Power and by restart, with and without the charger; (b) MISC `boot-recovery` alone (mtkclient) → expected normal boot, charge mode or power-off; (c) the flag survives a pulled cable on battery and is cleared by LK; (d) which button, if any, is keypad index 9 | RECOVERY (mtkclient), RTC PDN1, MISC (mtkclient) |
| **R2** | Kernel: the guard with a list of windows, the two trees, the PDN1 write exception, host tests for every boundary of both policies | none |
| **R3** | `core.c` Y1 geometry and device field, `package.py` Y1 profile, `y1_package.py` builds `RECOVERY.img`, size check; host tests with a Y1 `disk.img` (`Y2_UPDATE_TEST`) | none |
| **R4** | PID1 rescue and recovery modes, `recovery-ui` at 480×360 (host: built, [§10](#10-r4--pid1s-recovery-and-rescue--2026-11-12)). On the Y1: `probe-recovery` from the normal system (Linux writes MISC through the guard), a refused write outside each policy | BOOTIMG, RECOVERY (mtkclient), MISC (Linux) |
| **R4b** | The normal system on the Y1: the service daemon's update part (daily check, health ack) next to its network part, the update service's `/data` (Y2DATA on a loop over USRDATA, the Y1 UUID), `y1-update-core` as its core, `ota` off in the Y1's capabilities until R5 (host: built, [§11](#11-r4b--the-normal-systems-update-service--2026-11-12)) | none |
| **R5** | End to end from a local release: Update Now, install, health ack; then cancel, a pulled cable during the write (resume or restore), a failed health check (rollback), a stopped installer (watchdog restart), go back once | BOOTIMG, ANDROID, RECOVERY, MISC, USRDATA through the guard |
| **R6** | Release: INSTALL (RECOVERY written once, its backup, undo), a Y1 section in RECOVERY-AND-UPDATES, release notes, `ota` enabled for the Y1 | none |

## 4. Risks

- **LK has no usable door** (R1): R0 found the PDN1 flag in LK's code; if the probe does not confirm it, there is
  no automatic install and updates stay mtkclient-only. Decided before any code beyond the probe.
- **The flag is lost before the recovery re-arms it**: LK clears PDN1 in `boot_mode_select`, before its battery
  check; a start below LK's 3250 mV powers off and the flag is gone. MISC is still set, so the next normal start's
  rescue sees the interrupted install and requests the recovery again, but only if BOOTIMG still starts. The
  recovery therefore writes Y2ROOT first and BOOTIMG last (6 MiB, about a second), and only with the charger or half
  a battery. A torn BOOTIMG in that window needs mtkclient; on the Y2 MISC covers it.
- **A bad RECOVERY image**: the normal system does not need RECOVERY, so a broken image only stops updates; LK's
  behaviour with a bad recovery image while the request is set is measured in R0/R1 (a boot loop would need
  mtkclient). The probe writes MISC only after the RECOVERY readback matches.
- **Writing BOOTIMG or ANDROID wrongly**: every write is read back before the request is cleared, and the
  previous images are restored from Y2DATA; the preloader and mtkclient stay untouched as the last way back.
- **Wider guard**: each policy is tested at every boundary on the host (R2) and with a refused write on the device
  (R4); the normal system can still never write BOOTIMG or ANDROID.
- **6 MiB**: the recovery kernel may need its own config; then it is a second kernel build, checked like the first.
- **Y2DATA space**: the backups (about 42 MB system package, BOOTIMG) and the download need about 150 MB of the
  1.9 GB; music fills the same filesystem, so the Y2's space rule (old logs only, never music) applies.

## 5. Admission

| Stage | Item | Bound |
| --- | --- | --- |
| R0 | **Read** | reads on the Y1 and of the existing backups; offline analysis |
| R1 | **Probe the door** | the owner writes the probe to RECOVERY and the request to MISC with mtkclient, both backed up and read back first; a probe BOOTIMG writes only RTC PDN1 bits 5:4 |
| R2–R3 | **Build** | host code and tests only |
| R4 | **Recovery on the device** | BOOTIMG (its PID1 carries the rescue) and RECOVERY with mtkclient, both backed up and read back; Linux writes MISC through the guard; refused-write tests |
| R5 | **Install** | BOOTIMG, ANDROID, RECOVERY, MISC and USRDATA, only through the guard's policies; the owner's backup on the PC first |
| R6 | **Release** | documentation; publishing to Blunk/Y1Linux is the owner's step |

## 6. R0 — LK's recovery door — 2026-11-12

`evidence-private/2026-11-12-y1-recovery-r0/`. LK: `rea/lk/lk-thumb.dis` of the 3.0.2 `lk.bin` (base `0x80020000`, no
symbols); its strings are loaded PC-relative (`ldr rX, [pc, #n]` + `add rX, pc`), resolved into
`lk-string-refs.txt`. Function names INFERRED from their strings and MediaTek's reference LK; the control flow is
CONFIRMED in the code.

- **Order in `platform_init`** (`0x80020794` → `0x800207f4`): logo, backlight, **`boot_mode_select`**, sec init,
  download check, **`mt65xx_bat_init`**, RTC check.
- **`boot_mode_select`** `0x800219e8`: (1) a mode from the preloader (META etc., `0x800311f8`); (2) PDN1 bit 13 →
  fastboot; (3) factory (`0x80027588`); (4) the boot menu (`0x800319f8`: keypad index 0 within 50 ms opens LK's text
  menu); (5) **`recovery_detect`** `0x800311cc`; (6) **`kernel_power_off_charging_detection`** `0x80036470`,
  **called whatever (5) returned**. All of them write the same `g_boot_mode` (GOT `0x8005d0dc` → `0x8006041c`).
- **`recovery_detect`** sets `g_boot_mode = 2` if any of:
  - the RTC flag: `(PDN1 0x802c & 0x30) == 0x10` (`0x80028a74`);
  - a key: unless the PMIC "just reset" bit is set, keypad index 9 pressed within 50 ms (`0x800310f0`, `KP_MEM`
    `0x10002004` bit 9, active low); the stock kernel's keymap calls index 9 `KEY_VOLUMEUP`, the Y1's button for it
    is unknown;
  - MISC: `mboot_recovery_load_misc` reads `0x1800` bytes from the start of MISC, copies the first `0x440`
    (Android's `bootloader_message`) and `strcmp`s `command` with `boot-recovery` (`0x80031178`). LK never clears
    MISC.
- **The power-off charging check overrides it** (`0x80036470`). `is_force_boot` `0x800363f4`:
  - `rtc_boot_check(1)` `0x8002864c` returns 1 for an alarm, **or for the PDN1 recovery flag, which it clears**
    (`RTC_PDN1_FAC_RESET`, `PDN1 &= ~0x10`, `WRTGR`) → the mode is kept;
  - META → kept;
  - Power key held (`0x800276fc`) or preloader boot reason 4 (`0x80030a14`) → **`g_boot_mode = 0`, normal boot**;
  - otherwise not forced: a charger → mode 8 (charge mode), no charger → **power off**.
- **Consequence**: with only MISC (or only the key) set, LK never starts RECOVERY. A Power-key start and Linux's
  restart (reason 4) give a normal boot, a charger start gives charge mode, anything else powers off. Only the PDN1
  flag (or an RTC alarm start) carries mode 2 through, and LK clears that flag on the way. This is the stock
  kernel's own `reboot recovery` (`rtc_mark_recovery`, `out/y1-power-notes.md`). The Y2's LK (MT6582, a different
  build, `is_force_boot` at `0x81e166f4`) started RECOVERY from MISC after a Power press
  ([final 35](../validation/Y2-PLATFORM-V1-POWER-FINAL-35.md)), so the two LKs differ here.
- **`mt65xx_bat_init`** `0x80036344` runs after `boot_mode_select`: `rtc_boot_check(0)` again (the flag is already
  clear), then below 3250 mV (`0xcb1`) and not in charge mode, power off. A recovery start on an empty battery
  therefore loses the flag.
- **Loading** (`0x800380c8`): mode 2 loads partition `RECOVERY` (Android header, MTK `KERNEL` header; the stock
  image's ramdisk header is labelled `RECOVERY`, like the Y2 recovery image's).
- **RECOVERY and MISC**: from the backup (`read1/emmc_user.bin`, scatter offsets) and read on the Y1 (boot
  `481e8cef…`, kernel `6.18.56-y1linux`, `dd` from `mmcblk0` at `0x3180000`/`0x37c0000`), identical:

  | Partition | sha256 | Content |
  | --- | --- | --- |
  | RECOVERY (6 MiB) | `e9fe26a2…5da6fd` | the 3.0.2 `recovery.img` (5,214,208 bytes, `b5693f8e…`) followed by zeros |
  | MISC (512 KiB) | `89e78575…b2d92` | `bootloader_message` empty; MediaTek's LK environment at `0x20000` (`ENV_v1`, `data_free_size_th=52428800`); one sector at `0x23e00` |

  `core.c`'s 4 KiB message at the start of MISC stays clear of the environment at `0x20000`.

## 7. R1 — the RTC door on the Y1 — PASS — 2026-11-12

`evidence-private/2026-11-12-y1-recovery-r1/`, build `out/y1-recovery-r1` (`tools/build/y1-build.sh`).

- **Build**: one kernel, two trees. `RECOVERY.img` (`ae1132bb…3e28`, 4,179,968 bytes) differs from `BOOTIMG.img`
  (`5061596b…6b9f`) only in its tree's `y2.recovery=1` (checked by `tools/build/y1_package.py`), its ramdisk label
  `RECOVERY` and its name; `y1.dtb` is byte-identical to 9.3.24's. Kernel: overlay
  `0041-mt6323-poweroff-recovery-flag.patch` (the Y2's power-off patch 0027, before as 0030, plus the flag) adds
  `recovery_flag` (read 0/1; write sets or clears PDN1 bits 5:4 as stock `rtc_mark_recovery`, through `WRTGR`, read
  back) and `pdn1` (read-only) to `/sys/bus/platform/drivers/mt6323-pwrc/mt6323-pwrc/`. The Y1's PMIC write policy
  does not cover the RTC block, so no exception was needed. PID1 (`initramfs/y1-recovery.h`): with `y2.recovery=1`
  it shows *Recovery works*, skips the root hand-over and keeps its USB shell (new `poweroff` command).
- **Written by the owner** with mtkclient: RECOVERY and BOOTIMG, both read back on the player identical (first
  4,179,968 bytes). Normal start: PDN1 `0x0000`.

| Test | Start | Flag before | Result | PDN1 in the recovery |
| --- | --- | --- | --- | --- |
| 1 | Linux `reboot` (reason 4, which alone gives a normal boot) | set over SSH (`0x0000 -> 0x0010`) | RECOVERY: `/proc/cmdline` ends in `y2.recovery=1`, *Recovery works* | `0x0000` |
| 3 | Linux `poweroff` with USB in: the charger restarted the player (which alone gives charge mode) | set | RECOVERY | `0x0000` |
| 2 | 3 s centre hold off, unplugged; 5 s centre start on battery (Power held, which alone gives a normal boot) | set | RECOVERY | `0x0000` |
| — | each next start with the flag clear | clear | normal system, or the charge-only screen with USB in | `0x0000` |

The flag carries mode 2 through every way the Y1 starts, LK clears it at that start, and a start without it is
normal: R0's reading of LK holds. Not run: MISC `boot-recovery` alone (R0 shows LK cannot keep it; it would need a
MISC write and restore) and the keypad index 9 button (only visible on the UART). The probe stays in RECOVERY and the
flag interface in BOOTIMG until R4 replaces them; RECOVERY is reached only through the flag.

## 8. R2 — the guard by mode — 2026-11-12

Host code and tests only; nothing built here was written to the Y1.

- **Guard** (`kernel/y1/emmc-guard.h`, overlay 0034): `innioasis,emmc-write-window` is a list of one to four
  (first sector, sectors) pairs. A write must lie inside **one** window, so a write from BOOTIMG across into the
  adjacent RECOVERY (`0x18c00`) is refused even in the recovery's policy. The driver checks the list at probe
  (`y1_emmc_windows_valid`: one to four windows, none empty, none past sector 2^32, no overlap); a property that is
  present but invalid leaves the guard on with no window, so every write is refused. Everything else (no boot/RPMB
  write, no erase, the EXT_CSD switches) is unchanged.
- **The two trees** (`innioasis-y1.dts`), as the table in [§2](#2-design):
  `<0x267a00 0x400000 0x18c00 0x3000 0x1be00 0x400>` for BOOTIMG and, with `Y1_RECOVERY`,
  `<0x267a00 0x400000 0x15c00 0x3000 0x22a00 0x145000 0x1be00 0x400>`. `y1_package.py` requires each tree's list
  and that the recovery tree differs only in `y2.recovery=1` and its list.
- **PID1** (`y1-root.h`): attaches Y2DATA when the list is one to four pairs led by exactly USRDATA
  (`guard_covers_usrdata`, in the QEMU selftest), instead of the single USRDATA window.
- **PDN1 write exception: not needed.** The Y1's PMIC write policy (overlay 0005) covers only the charger block,
  ISINK and the audio registers; the RTC block (`0x8000`…) is outside it, which is why R1's flag write worked.
- **Host tests** (`tests/test_y1_emmc_guard.py`, the partitions from the 3.0.2 scatter): for each policy, every
  window is exactly its partition (first and last sector and the whole partition writable; one sector past either
  end, a write across into the neighbour and one sector too many refused); every other partition (MBR/EBR1,
  PRO_INFO, NVRAM, PROTECT_F/S, SECCFG, UBOOT, SEC_RO, LOGO, EXPDB, CACHE, FAT, and BOOTIMG/ANDROID resp. RECOVERY)
  refused at its first, middle and last sector; the tree's lists equal the policies and `y1_package.WINDOWS`; the
  list validation at its limits. Three mutations of the guard (overlap check, window end, zero blocks) each fail
  the suite.

## 9. R3 — the Y1's updater and packages — 2026-11-12

Host code and tests only; nothing written to the Y1.

- **One source, two binaries** (`buildroot/package/y2-update`): `y2-update-core` as before and `y1-update-core`
  (`core.c` with `-DY2_UPDATE_Y1`), both in Y2ROOT, which both players run. The choice is made at build time; the
  Y1's PID1 and `S42` (R4) call `y1-update-core`. `tools/production/validate.py` checks both as ARM binaries
  without fixture code.
- **The Y1's geometry** in `core.c`: disk `7569408` sectors on `11120000.mmc`; BOOTIMG `0x15c00`, RECOVERY
  `0x18c00`, MISC `0x1be00` (sectors); boot images 6 MiB; the root limit is ANDROID, 650 MiB. Y2ROOT is written
  through a **loop device over exactly ANDROID** (autoclear, created by the core on the disk it identified), so the
  root writer is unchanged and its writes reach the eMMC only through the recovery tree's guard window; refused
  while any loop device of the disk sits at ANDROID's offset (the running system's root).
- **Packages name their player**: the Y1 images are `Y1Linux-M1` / `Y1Recovery-1` (`tools/build/bootimage.py`
  `NAMES`, built by `y1_package.py`, checked by `tools/validation/bootimg.py`); a Y1 manifest carries
  `"device": "y1"` (`package.py --device y1`), a Y2 manifest no field. Each core refuses the other's: the device
  field ("incompatible device") and the image names. `product`/`hardware_revision` stay the root image's (the Y2ROOT
  compat file both players carry).
- **The door** (`RECOVERY_FLAG`, overlay 0041's `recovery_flag`): the flag is set with every MISC request and
  cleared with it, read back each time. If setting it fails, the message is taken back: there is never a request
  without the flag. The recovery **re-arms** the flag as soon as it reads a request, so an interrupted install
  returns to the recovery; the request and flag are cleared together only after the outcome is verified. A start
  that lost the flag but kept MISC (below LK's 3250 mV) reaches the rescue, which asks again as on the Y2 (three
  times at most). `y1-update-core misc` reports `recovery_flag`.
- **Recovery environment**: the installer writes only with the recovery initramfs marker and `y2.recovery=1` as a
  whole word of `/proc/cmdline` (the Y2's: `recovery_image=1` in its boot metadata).
- **Size**: the installer for RECOVERY's initramfs (`y1-update-core`, `y2-recovery-ui`, `ld-linux`, glibc,
  libsodium, json-c, zlib, libdrm from the Beta 9.3.24 Y2ROOT) is 2,416,464 bytes, **1,180,306 gzip-compressed**.
  RECOVERY.img is 4,182,016 bytes today, so with the installer about 5.36 MB of 6,291,456: no separate recovery
  kernel config. `y1_package.py` keeps `INSTALLER_BYTES` (1.25 MiB) free in RECOVERY.img. The installer itself goes
  into the initramfs in R4, with PID1's recovery mode.
- **Host tests** (`tests/test_platform_update.py`): the whole update suite runs a second time as `Y1UpdateTests` on
  `y1-update-core` with a Y1 `disk.img` (install, interrupted installs, rollbacks, kept packages, lost requests,
  probes, faults). LK is modelled: every start clears the flag, and after every run of the core the flag is set
  exactly while MISC holds a request. Y1 cases: the flag re-armed after an interrupted install; a lost flag with
  MISC kept; no flag interface means no request; Y1 names, 6 MiB limit and the Y2 image refused by `package.py`.
  Both cores refuse the other player's manifest.
- **Left for R4**: the battery check reads `BAT0` and the USB/Mains supplies as on the Y2 (the Y1 uses the same
  charger driver); the update service's `/data` lookup; `y2-recovery-ui` at 480×360; `capabilities.json` keeps `ota`
  off for the Y1.

## 10. R4 — PID1's recovery and rescue — 2026-11-12

Host code, a build and the selftest; nothing written to the Y1. Build `out/y1-r4-boot` against a Y2ROOT target
that is 9.3.24's plus `y1-update-core` compiled from this tree with its toolchain (`out/y1-r4-y2root`, a hard-linked
copy; the 9.3.24 root predates R3).

- **The installer is in the built-in initramfs, in both images** (`tools/build/y1_installer.py`, a second
  `CONFIG_INITRAMFS_SOURCE` list made by `y1-build.sh` from the Y2ROOT build named as its argument): `y1-update-core`,
  `y2-recovery-ui`, their loader and libraries, the trust and compatibility files and the recovery marker. Not in
  RECOVERY's separate ramdisk: this kernel never sees LK's ramdisk (ATAGs off, appended tree). Not only in RECOVERY:
  the BOOTIMG rescue runs the core before the root is mounted, so a torn Y2ROOT cannot stop it, as on the Y2.
  `y1_package.py` checks that the kernel's initramfs holds every listed file byte for byte (`installer_bytes`
  2,869,136) and drops R3's 1.25 MiB reservation. INSTALL now builds Y2ROOT before BOOTIMG.
- **Size**: with e2fsck and its five libraries both images were 6,199,296 bytes, 92 KB under 6 MiB. XZ and LZMA would
  save 0.5 MB, but the build userspace has neither compressor (its `lzma` is BusyBox's, decompress-only); bzip2
  saves 50 KB. Without e2fsck: **5,658,624 bytes, 618 KiB left**. PID1 mounts Y2DATA only when its superblock is the
  Y1's Y2DATA (ext4 magic, label, UUID `79314c69-…0102`) and records no error (`s_state` bit 1); the mount replays
  the journal, the part of `e2fsck -p` an unclean stop needs. `S03y1-data` still runs `e2fsck -p` at every normal
  start.
- **PID1** (`initramfs/y1-update.h`, the Y2's `recovery.sh` and init rescue in C; Y2DATA is now attached before the
  root):
  - *Recovery* (`y2.recovery=1`): `/run` on tmpfs, the screen started, `/dev/watchdog` opened; Y2DATA checked and
    mounted at `/newdata` (else *Update needs attention*: *Storage not found* / *Storage check failed*);
    `restart.json` read; `y1-update-core recovery` with the watchdog fed only while `/run/y2/recovery-progress`
    changes within 120 s (300 s for the steps before); `needs_power` waits ten minutes for a charger, else turns off;
    each outcome shows the Y2's screen, keeps `recovery-last.json` and `logs/recovery-last.log` on Y2DATA, unmounts,
    darkens the screen and restarts (or turns off with `{"mode": "poweroff"}`). An unknown outcome keeps the request,
    hands the watchdog back (`V`) and stays in PID1's session with the USB shell; after ten minutes without a charger
    it turns off. The R1 probe screen is gone; the `y2-probe` request is the core's (*Recovery works*).
  - *Rescue* (BOOTIMG, before `prepare_root`): with `updates/state.json` on Y2DATA, `boot-needed` on a read-only
    mount, then `boot-check` on a checked read-write mount (`boot-check-last.json` kept). 10 starts the system, 20
    (the core set the request and the flag) restarts into RECOVERY: unlike the Y2, which powers off, the flag carries
    a restart there (R1, test 1). Anything else stays in PID1's session with *an update needs attention*.
  - Selftest (`Y1INIT SELFTEST OK`): the outcome and `restart.json` parsers, the superblock check (the Y2's UUID, a
    longer label, the error bit and a wrong magic refused).
- **The screen**: `recovery-ui.c` already draws a 480×360 frame centred on the panel, and the Y1 panel's mode is
  480×360 (`kernel/y1/panel-lk.c`); the preview renders the Y1's new texts (*Update needs attention*, *Turning off to
  save power.*) inside its 28 columns. No change.
- **Host tests** (`tests/test_y1_installer.py`): the cpio reader; the image check passes an exact copy and refuses a
  changed file, a missing file and a list without the core; with a Y2ROOT build in `out/`, the list from it (loader
  included, no e2fsck) and a core with test fixtures refused.
- **Gate**: fmt, clippy, armv7, the code-quality ratchet and cargo test pass; the 20 platform goldens
  (`report-upload-*`, `reset-plan-*`, `media-status-*`) fail as in R2 and R3. No platform code changed.
- **The device run (owner)**: a Y2ROOT built from this tree (it carries `y1-update-core`), BOOTIMG and RECOVERY from
  `y1-build.sh` with it, each written with mtkclient and read back. Then from the normal system: `mkdir -m 700
  /data/updates` if absent, `y1-update-core misc` (no request, flag 0), `y1-update-core probe` (MISC `y2-probe`, flag
  1, read through the guard's MISC window), `reboot` → RECOVERY shows *Recovery works* and restarts into the system;
  `/data/updates/recovery-probe.json` names the recovery's boot, `y1-update-core misc` shows no request and flag 0,
  `/data/logs/recovery-last.log` holds the run. Refused writes from the normal system: one sector at BOOTIMG
  (`0x15c00`) and at ANDROID (`0x22a00`) on `mmcblk0` fail and the guard logs them; one at MISC passes (then
  restored).

## 11. R4b — the normal system's update service — 2026-11-12

Host code and tests; nothing built here was written to the Y1. The Y2ROOT image is shared, so the platform tells
the players apart at runtime: `y2p_core::player` reads the device tree's first compatible string (`innioasis,y1`;
anything else, and a fixture root, is the Y2).

- **The core**: every update action runs `Player::update_core()`, `/usr/sbin/y1-update-core` on the Y1
  (`update/journal.rs`). Nothing else of the update service changed: the battery gate reads the power daemon's
  record as on the Y2, and the restart goes through the power daemon (which the Y1 runs since D2).
- **`/data` and the root** (`y2p_core::volume::identity_source`, also used by the observation snapshot): on the
  Y1, Y2DATA is verified as its loop over exactly USRDATA (sysfs `loop/offset` `1291059200`, `sizelimit`
  `2147483648`, `backing_file /dev/mmcblk0`, `/sys/block/mmcblk0` below `11120000.mmc`) with the Y1's UUID
  (`79314c69-…0102`). The root is an overlay (a RAM upper layer) whose read-only lower mount PID1 hid with the
  root switch: it is verified when `/` is an overlay and exactly one read-only loop covers exactly ANDROID
  (`72613888+681574400`), and `blkid` reads the Y2ROOT UUID from that loop. Loop numbers vary between starts
  (the device run had Y2DATA on `loop0`, ANDROID on `loop1`); nothing relies on them. Reborn's RAM `/data` (an image
  on another loop) never verifies. So `update check`/`stage` find their transfer volume and the health
  acknowledgement sees `root` and `data` OK; `render_node` (Lima), `display`, `audio` and `input` exist on the Y1.
- **The root's space**: `statvfs("/")` is the overlay's tmpfs upper layer. PID1 made it 32 MiB, 30 MiB free, which
  the platform's budget reads as CriticalSpace (under 96 MiB free is LowSpace), so the root was DEGRADED and the
  health acknowledgement could never pass. PID1 now mounts it with `size=128m`, a limit, not a reservation.
- **`S03y1-data`** writes the mounted loop to `/run/y2-data-device`, as the Y2's initramfs writes its partition,
  and creates `/data/updates` (0700). With it the platform's durable records count Y2DATA as verified: the boot
  journal (`boot-stage`, which the health acknowledgement records `application_ready` in) and the power daemon's
  sleep diagnostics now persist on the Y1 as on the Y2. `boot-stage application_ready` arms the Y2's runtime power
  features only on the Y2: the Y1's kernel has none of their switches.
- **The service daemon**: `/etc/y2linux/service-policy.json` is now `{"schema": 1, "network_only": true,
  "updates": true}` (moved to `initramfs/y1-update`). With schema 1 and `"updates": true` exactly, the network part
  gains the update part: the daily `update auto-check` once Wi-Fi is online (manifest only, never a download) and
  `update health-ack` on the maintenance passes until no update waits for health. Media, the SD card, the radio
  firmware scan, the space cleanup and error reports stay off; an older platform reads the same file as network
  only (W6). Without the key, or with another schema, the service is W6's.
- **`S42y2-readiness`** (Y1 override, `initramfs/y1-update`, in `y1-services` after `S41`): starts the Y2's
  readiness supervisor only on the Y1's own Y2DATA (`/run/y2-data-device` mounted at `/data`) and only with a
  platform that knows the policy; refuses during a reset as the Y2's. It owns the service daemon now, with or
  without Wi-Fi, since an installed update needs its health acknowledgement either way: `S41y2-connectivity` no
  longer starts or stops it, so its restart after a firmware import leaves it running.
- **Capabilities**: `tools/build/y1_capabilities.py` (run by `y1-build.sh` from the Y2ROOT build) writes the root's
  `capabilities.json` with `ota` and `automatic_bootimg_update` disabled (reason: R5), nothing else changed; PID1
  installs it over the root's file. R6 dropped the override ([§13](#13-r6--release--2026-11-13)).
- **Until R5**: the Blunk/Y1Linux release 9.3.24 has no `"device": "y1"` manifest, so the Y1's daily check records a
  failed check (the core refuses the manifest) until a Y1 package is published; it offers nothing to install.
- **Tests**: `y2p_core` (`player`, the Y1 volumes: Ready as set up; another window, another backing file, the Y2's
  Y2DATA UUID and the same mounts on the Y2 refused), `observation/health_tests.rs` (the Y1's `root` and `data`
  OK), `update.rs` (the Y1 asks `y1-update-core`), `tests/service.rs` (the update part's two children with the
  key; none with a non-boolean key or another schema), `tests/test_y1_update_service.py` (policy, the PID1 and
  list entries, S42's start conditions run in a shell, S03's record after the schema check, the capabilities
  derivation), `tests/test_y1_connectivity.py` (S41 leaves the daemon to S42).
- **Gate**: fmt, clippy, armv7, the code-quality ratchet and cargo test pass; the platform goldens fail as in R2–R4
  (20 differential cases, `report-upload-*`, `reset-plan-*`, `media-status-*`, and 5 in the reports and observation
  suites), each identical on the tree before R4b.
- **On the device (with R4's run)**: after writing the images, `/run/y2-data-device` names the USRDATA loop,
  `y2-platform health` shows `root` and `data` OK, `pgrep -f service-daemon` runs with Wi-Fi off,
  `y2-platform capabilities --json` shows `ota.enabled` false, and with Wi-Fi online
  `/data/updates/last-check.json` records an attempt.

### R4's device run: R4b on the Y1 — 2026-11-12

`evidence-private/2026-11-12-y1-recovery-r4/`. Before any write (`before/`, read over SSH): BOOTIMG, RECOVERY and
MISC (= R0) backed up, and the root image (`0758be82…`, an earlier 9.3.24 build of `77073a76` that `out/` no
longer holds) as `ANDROID-512MiB.img.gz`, each checked against the device. The owner wrote ANDROID
(`out/y1-root-r4b`, built from `5fe02e41`), BOOTIMG and RECOVERY (`out/y1-r4b-boot`, 5,664,768 bytes each) with
mtkclient; all three read back identical on the player (`5d64b3b1…`, `9d49f77d…`, `0bd7e244…`).

- The normal system started: `S03y1-data` recorded `/dev/loop0` in `/run/y2-data-device` and created
  `/data/updates`; `S42y2-readiness` started the service daemon (policy with `updates`); the capabilities report
  `ota` and `automatic_bootimg_update` disabled; `y1-update-core misc` answered `Idle`, no request, flag 0;
  `last-check.json` recorded an attempt; `data` OK.
- **Found**: `root` FAILED (`source_missing_or_wrong_controller`): the root is the overlay, not the loop the host
  fixtures had assumed. With the fix above, a cross-built platform run from `/tmp` read `root` as found but
  DEGRADED (CriticalSpace, the 32 MiB upper layer). Both fixed; the fixtures now carry the device's mountinfo.
- **PASS** with the images from `6e81b162` (ANDROID `a2781fd8…`, BOOTIMG `61f28a1e…`, RECOVERY `453d8232…`, written in
  one mtkclient command, all read back identical): `/` 128 MiB, 126 MiB free; `root`, `data`, `filesystem_errors`,
  `render_node`, `display`, `audio` and `input` OK; the service daemon running; `y1-update-core misc` Idle, flag 0.
- **The probe** (R4): `y1-update-core probe` from the normal system wrote MISC (`boot-recovery`, request `y2-probe`)
  through the guard's MISC window and set the flag (PDN1 `0x0010`); `reboot` started RECOVERY (its own boot),
  which logged `recovery started`, set the flag again, ran the probe (`step=probe 100`), cleared flag and request
  together and restarted into the normal system: `recovery-probe.json`, `recovery-last.json` (`outcome: probe`) and
  `logs/recovery-last.log` on Y2DATA, MISC without a request, flag 0.
- **The guard** (R4): a sector rewritten with its own content from the normal system: BOOTIMG (`0x15c00`) and
  ANDROID (`0x22a00`) refused (`refused CMD24`, nothing written), MISC and RECOVERY written; all four unchanged.
- Minor, left for R5: `recovery-probe.json`'s `metadata` is `/proc/cmdline` cut before `y2.recovery=1`; the
  recovery's files are dated 1970 (no clock source in the recovery).

## 12. R5 — install, cancel, failed health, go back — 2026-11-12

`evidence-private/2026-11-12-y1-recovery-r5/`. A test release 1.0.0-beta.9.3.25 (branch `y1-r5-test`: only
`release.json` and the Y2 kernel's version string; never merged or published), sequences 16001-16004 signed with
the Y1 release key (owner's decision), staged from `/data/r5` with `update stage --package`. `ota` stays off in the
image; for each test it was enabled only in the running system's RAM overlay, so Reborn offered its buttons (Reborn
grants Install, Cancel and Restore only with `ota` enabled: R4b's gate works).

- **Install (16001)**: Install Now wrote RECOVERY, restarted; the recovery verified, backed up root and BOOTIMG,
  wrote Y2ROOT then BOOTIMG (about 95 s), cleared request and flag. **Found**: PID1 read `recovery-result.json`
  into 1 KiB; the core's record (the whole journal with the backup's segment list, about 4 KiB) has `outcome`
  last, so the player stopped on *Update needs attention*. A restart from the USB shell started 9.3.25, which
  acknowledged its health at 54 s. Fixed in `ebb3a003` (32 KiB, selftest with a 650-segment record).
- **Cancel (16002)**: the core recorded `Failed` / `owner cancelled before installation` and removed `pending/`
  (`update cancel` exit 0). Reborn shows *The operation did not finish*: its task rule counts any `state: Failed`
  reply as a failure, on the Y2 as well (Y2Reborn `client.rs`), not fixed here.
- **Install with the fix (16002)**: `outcome: installed`, the recovery restarted by itself, Acknowledged at about
  50 s, accepted floor 16002.
- **Failed health (16003)**: Reborn stopped on the new system; at the 180 s deadline the health check set
  `RollbackPending` with MISC `y2-update` and the flag; the next start restored 16002 (`RolledBack`, `outcome:
  rolled_back`) and restarted by itself; `update` health DEGRADED with the restore's reason.
- **Go back once (16004, rollback allowed)**: installed and acknowledged; Service → Diagnostics → Restore Previous
  System restored 16002 (`RolledBack`, `current_sequence` 16002, highest 16004: 16004 cannot be replayed).
- **Not run, accepted by the owner (2026-11-13)**: an interrupted write and a stopped installer. Holding the
  centre button does not reset the player in the recovery, and PID1 serves no shell while the installer runs, so
  neither can be caused from outside. Both paths are the host suite's (`Y1UpdateTests`: interrupted installs, the
  flag re-armed, a lost flag with MISC kept) and the Y2's device runs.
- **Cleaned up (2026-11-13)**: the four signed test packages deleted from `out/`, from `/data/r5` and the core's
  kept copy of 16004 (`/data/updates/current`) on the player. The player stays on the test release (16002,
  `RolledBack`); the highest accepted sequence is 16004, so the next release needs a higher one (`release.py`'s
  default +100 from 16000 is 16100). The Reborn cancel message is fixed in Y2Reborn `efb1df7` (branch
  `fix-cancel-result`, for upstream), on `y1` as `29c308a`.

## 13. R6 — release — 2026-11-13

No image built for the player and nothing written to it. Closing audit:
[roadmap/gap audit](roadmap-gap-audit.md#y1-recovery-r6-ota-on-closing-audit--2026-11-13).

- **`ota` on**: R4b's override is gone (`tools/build/y1_capabilities.py`, its `y1-build.sh` step, the
  `y1-production.list` entry, PID1's override entry), so the Y1 reports the root's `capabilities.json`: `ota` and
  `automatic_bootimg_update` enabled, as R5 had them in the RAM overlay. Its descriptive fields (`installer`:
  "Y2 recovery image … after power-off; MISC boot-recovery request") still name the Y2's door; no platform code
  reads them. `tests/test_y1_update_service.py` checks that no override is left; PID1 builds and its selftest
  passes (`Y1INIT SELFTEST OK`, locked build userspace).
- **INSTALL**: RECOVERY in the backup, written once with the other images, the release files as an alternative to
  building; *Updating* over Wi-Fi, the first update of a player installed before (RECOVERY, BOOTIMG and ANDROID once
  with mtkclient), the update by hand, the two paths not tested on the Y1; the way back with `recovery-stock.img`,
  and RECOVERY alone back to stock. Found while writing it: stock RECOVERY does **not** stop updates. The core
  writes the package's recovery image whenever RECOVERY differs (`core.c`, the `Queued` request), so only Restore
  Previous System and the rollback before the next update need it; the Y2's page says otherwise and is left as
  upstream's.
- **Recovery and updates**: a section *On the Y1* (the RTC door, the rescue restarting, mtkclient, the guard,
  sizes, its own releases, what was tested).
- **Release notes**: [draft](../release/Y1LINUX-OTA-RELEASE-NOTES.md) in the
  [release notes format](../RELEASE-NOTES-TEMPLATE.md), with VERSION and SEQUENCE to fill in.

### Making a Y1 release (the owner's step)

`release.py build` makes the Y2's release (SP Flash Tool package, Y2 kernel) and `bump` writes a `Y2LINUX-` build
ID, so a Y1 release is made by hand, as 9.3.24 was:

1. **Identity**: `tools/production/release.json`: `release_version`, `build_id` `Y1LINUX-…`, `rootfs_version` one
   up, and `update_sequence` **at least 16100**: the owner's player accepted test sequences up to 16004 in R5, and a
   lower or equal sequence is refused. Reborn's version as `release.py bump` sets it. Commit, tag
   `y1linux-<version>` in Y2Linux and Y2Reborn.
2. **Build** from that commit ([INSTALL](../INSTALL.md#build-the-images) steps 2 and 3): `out/y1-root-<version>`
   (Y2ROOT with `y1-update-core`, `versions.json`), then `out/y1-boot-<version>` from it (`BOOTIMG.img`,
   `RECOVERY.img`).
3. **Sign**:

   ```sh
   python3 tools/update/package.py package --device y1 \
       --image out/y1-root-<version>/buildroot/images/rootfs.ext4 \
       --bootimg out/y1-boot-<version>/BOOTIMG.img --recovery out/y1-boot-<version>/RECOVERY.img \
       --versions out/y1-root-<version>/versions.json --sequence <update_sequence> --rollback-allowed \
       --key-id y1linux-release-20261112 --private ~/.local/share/y1linux/signing/y1linux-release-20261112.key \
       --output out/y1-ota-<version>
   ```

   `--rollback-allowed` lets Restore Previous System go back from this release, as the Y2's releases do.
4. **Check on the player** before publishing (R5's way): copy the package to `/data/r6`, `y2-platform update stage
   --package /data/r6`, Install Now, and see it acknowledged. That consumes the sequence on this player only.
5. **Publish** to Blunk/Y1Linux as the **latest** release (the player reads
   `releases/latest/download/manifest.json`): `manifest.json`, `manifest.sig`, `rootfs.ext4.gz`, `bootimg.img.gz`,
   `recovery.img.gz` from step 3, `BOOTIMG.img` and `RECOVERY.img` from step 2 for mtkclient, `SHA256SUMS` over all
   seven. The update's `rootfs.ext4.gz` is one gzip stream of the exact image, so it is the mtkclient download too.
   The text: the draft with VERSION and SEQUENCE filled in.
