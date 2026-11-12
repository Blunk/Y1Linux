# Y1 recovery image and over-the-air updates

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The design follows the
> Y2's [recovery and updates](../RECOVERY-AND-UPDATES.md) and [platform OTA v2](../architecture/platform-ota-v2.md).

Status: **R0 done, R1 PASS** (2026-11-12, [R0](#6-r0--lks-recovery-door--2026-11-12),
[R1](#7-r1--the-rtc-door-on-the-y1--pass--2026-11-12)): the RTC PDN1 recovery flag starts RECOVERY on the Y1 by a
Linux restart, a centre-button start and a charger start, and LK clears it each time. R2 (guard by mode) next. The
release channel (GitHub Releases of Blunk/Y1Linux) and
the Y1 release key (`y1linux-release-20261112`, `tools/update/trust.json`) are in place since `ea684047`. Today the
Y1 is installed and updated only with mtkclient ([INSTALL](../INSTALL.md): "there are no over-the-air updates on the
Y1"). The goal is the Y2's chain on the Y1: a Y1 recovery image in RECOVERY, written once with mtkclient, that
installs signed updates (BOOTIMG, Y2ROOT, RECOVERY) and restores the previous version if anything fails.

This widens the Y1's write scope from USRDATA alone to BOOTIMG, ANDROID, RECOVERY and MISC. Before R2 the
[roadmap/gap audit](roadmap-gap-audit.md#standing-milestone-boundary-rule) is repeated with R0/R1's hardware
evidence.

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
  (`y2-update-core`, `y2-recovery-ui`, zlib) has to fit in the rest. Measured in R3; if it does not fit, the recovery
  kernel config drops what the installer does not need (audio, Bluetooth, Wi-Fi, GPU).

## 3. Stages

| Stage | What | Writes |
| --- | --- | --- |
| **R0** | Offline: disassemble the 3.0.2 LK's boot-mode decision (MISC, RTC PDN1, keys; what it clears, what it does with a bad image); back up RECOVERY and MISC from the existing dump; read both on the Y1 and compare | none to the Y1 (**done**) |
| **R1** | Probe image: the current kernel with a recovery tree and a PID1 that only logs `boot_mode`, shows *Recovery works* and powers off. Written to RECOVERY with mtkclient, read back. Then, on the UART: (a) the PDN1 flag set from a probe BOOTIMG build (one RTC write, the stock sequence) → LK starts RECOVERY, by Power and by restart, with and without the charger; (b) MISC `boot-recovery` alone (mtkclient) → expected normal boot, charge mode or power-off; (c) the flag survives a pulled cable on battery and is cleared by LK; (d) which button, if any, is keypad index 9 | RECOVERY (mtkclient), RTC PDN1, MISC (mtkclient) |
| **R2** | Kernel: the guard with a list of windows, the two trees, the PDN1 write exception, host tests for every boundary of both policies | none |
| **R3** | `core.c` Y1 geometry and device field, `package.py` Y1 profile, `y1_package.py` builds `RECOVERY.img`, size check; host tests with a Y1 `disk.img` (`Y2_UPDATE_TEST`) | none |
| **R4** | PID1 rescue and recovery modes, `recovery-ui` at 480×360, Y1 `S42`, the update service's `/data` lookup. On the Y1: `probe-recovery` from the normal system (Linux writes MISC through the guard), a refused write outside each policy | RECOVERY (mtkclient), MISC (Linux) |
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
| R4 | **Recovery on the device** | RECOVERY with mtkclient; Linux writes MISC through the guard; refused-write tests |
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
