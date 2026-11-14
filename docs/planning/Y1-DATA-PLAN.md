# Y1 persistent data: Y2DATA (system milestone B)

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The step builds named
> below (the *N*th boot, its variant, tree, fragment and PID1 header) are the record of the test images that ran
> on the player; their files are folded into that build.

Status: **B0 done, B1 PASS, B2 PASS** (2026-11-05, owner-approved; [B2 run](#9-b2-run--pass--2026-11-05)); B3 (music) next.
**B4** (the built-in card as Internal Storage, 2026-11-14): built, not yet run on the player ([§10](#10-b4--the-built-in-card-as-internal-storage--2026-11-14)). The owner chose milestone B after D2. Today `/data` is a 64 MiB
ext4 image in RAM (`S90y1-reborn data`, run early by `S03y1-data`): Reborn's settings and library, the power
daemon's journals and the SSH host keys are lost at every boot, and there is nowhere to keep music. Each stage
has its own admission; B1 is the only destructive write.

## 1. Evidence

Offline (agent notes `out/y1-data-notes.md`; read-only reads of the private backup). CONFIRMED unless marked.

- **The eMMC** (`evidence-private/2026-11-04-y1-backup/00-printgpt.txt`, the 3.0.2 scatter; physical = scatter −
  0x600000): **USRDATA** scatter `0x4d540000`, physical `0x4cf40000`, **2 GiB** (`0x80000000`); CACHE 512 MiB at
  physical `0x2cf40000`; FAT 396 MiB at `0xccf40000`; everything else stock-critical (preloader, MBR/EBR1,
  PRO_INFO, NVRAM, PROTECT_F/S, SECCFG, LK, RECOVERY, SEC_RO, MISC, LOGO, EXPDB). ANDROID holds Y2ROOT since the
  rootfs stage, so stock Android no longer boots.
- **USRDATA today** (backup): stock Android `/data`, ext4, about 94 MiB used (`BT_Addr`, `nvram/`, `misc/`);
  RECOVER flag set. The live partition has probably changed since (the ADB evidence boot wrote to it, INFERRED).
- **Backups**: `read1/emmc_user.bin`, the full logical dump except the last 6 MiB (sha256 `796cdf27…3497ef`);
  `read2/first-49MiB.bin`; `ANDROID-from-backup.img`. **The second-drive copy is still owed**
  (`MANIFEST.txt:26`), and a stock restore has never been proven.
- **How the Y2 does it**: Y2DATA is never created on the device. `tools/production/package.py` builds it offline
  (`mkfs.ext4 -L Y2DATA`, fixed UUID, a seed with `.y2data-schema` = 1, directories and the owner's
  `authorized_keys`); it is flashed to USRDATA once. At boot the rescue initramfs finds the partition (controller,
  start, size, label, UUID), checks it read-only, runs `e2fsck -p` (anything above 1 goes to rescue, never to
  mkfs) and mounts it `rw,nosuid,nodev`; `S02y2-data` creates the directories. The Y2 kernel has a write guard
  (`kernel/platform/storage-policy.h`) limiting eMMC writes to known ranges; **the Y1 kernel has none**.
- **How the Y1 reaches the eMMC**: Linux sees `mmcblk0` without partitions (no GPT parser for the MTK table).
  Y2ROOT is a read-only loop on `mmcblk0` (offset `0x4540000`, sizelimit `0x28a00000`); PID1 sets every disk
  read-only (`BLKROSET`). A writable USRDATA loop needs `mmcblk0` writable, which a loop's size limit does not
  bound for other writers.
- **`mmcblk1`** is a built-in 116 GiB FAT32 card with the owner's files (stock's music storage). Reborn's
  `/media/sd` is tied to the Y2's controllers. The card sits inside the case, so the owner cannot swap it: since B4
  it is the Y1's **Internal Storage** at `/media/internal` (read-write), never an SD card ([§10](#10-b4--the-built-in-card-as-internal-storage--2026-11-14)).
- `S02y2-media` (can format the stock FAT) and `S42y2-readiness` (MTP, raw MISC write) stay off.

## 2. Design

- **Location**: USRDATA, all 2 GiB, one ext4 filesystem (label `Y2DATA`, a fixed Y1 UUID), through a loop at
  offset `0x4cf40000`, sizelimit `0x80000000`.
- **A Y1 eMMC write guard** in the kernel, after the Y2's `storage-policy.h`: writes to `mmcblk0` are refused
  except inside USRDATA (sectors `0x267a00`–`0x667a00`). With it, `mmcblk0` can be writable for the loop without
  exposing the stock-critical partitions or Y2ROOT. Unit-tested on the host.
- **Creation offline, like the Y2**: a seed image built on the PC (`mkfs.ext4 -L Y2DATA`, `^64bit`, the schema
  file, `ssh/authorized_keys.d/authorized_keys` from `out/y1-ssh`, `ssh/host-keys`, `reborn/state` with the
  seeded settings, `music`, `system/platform`), flashed by the owner with mtkclient and read back. Never mkfs on the
  device. The power daemon's override files under `/data/system/platform` are not seeded (they would override
  the Y1's profile and policy in `/etc`).
- **Boot**: PID1 attaches the loop, checks label, UUID and schema read-only, runs the image's `e2fsck -p` (0 or 1
  continues; anything else keeps the RAM `/data` and reports it), mounts `rw,nosuid,nodev,noatime` at `/data`
  before `switch_root`. The RAM `/data` stays as the fallback. `S03y1-data` creates the directories (as the Y2's
  `S02y2-data`); shutdown syncs and unmounts.
- **Music**: `scp`/`sftp` over USB networking into `/data/music` (rsync is not in Y2ROOT; MTP out of scope).

## 3. Stages

| Stage | What | Writes |
| --- | --- | --- |
| **B0** | The owed second-drive backup copy (owner); two mtkclient reads of USRDATA, hashes compared; a read-only superblock check on the Y1; the write guard and its test; the seed image built | none to the Y1 |
| **B1** | Flash the seed image to USRDATA with mtkclient; read back; digests of the neighbouring partitions before and after | USRDATA only, exactly `0x80000000` bytes at scatter `0x4d540000` |
| **B2** | New BOOTIMG: the guard, the PID1 mount, the data script. Tests: a file and the SSH host key survive a reboot; `e2fsck -p` returns 0/1 after a pulled cable on battery; a raw write outside USRDATA is refused | the Y2DATA filesystem through the loop |
| **B3** | Music over `scp`/`sftp`, Reborn scans and plays it after a reboot | files in `/data/music` |
| **B4** | The built-in card as Internal Storage: `S03y1-media` mounts it at `/media/internal`, Reborn and MTP use `/media/internal/music` | the owner's FAT on `mmcblk1` (files only, never formatted) |

## 4. Risks

The destructive write lands in the wrong place (B1 writes by partition name with mtkclient, reads back, and
compares the neighbours). Linux writes outside USRDATA (the guard; until B2 nothing is writable). Power loss
during a write (ext4 journal, `e2fsck -p` at boot, the RAM fallback). The stock `/data` is lost (it is in the
backup; stock Android already does not boot). eMMC wear (small: `noatime`, logs stay in RAM). Recovery: the
preloader and mtkclient are untouched; USRDATA can be written back from the backup.

## 5. Admission

| Stage | Item | Bound |
| --- | --- | --- |
| B0 | **Prepare** | reads only on the Y1; a backup copy; host-side code and image |
| B1 | **Write USRDATA** | the seed image, by the owner with mtkclient, USRDATA only, read-back checked |
| B2 | **Mount Y2DATA** | the loop over USRDATA only, behind a kernel guard refusing every other eMMC write; no mkfs on the device |
| B3 | **Copy music** | files into `/data/music` over the USB network |
| B4 | **Card read-write** | `mmcblk1` and its FAT partition only, mounted `vfat` with `errors=remount-ro`; the eMMC stays behind its guard; no mkfs, no fsck repair |

## 6. B0 — 2026-11-05

The owner declined the second-drive copy ("there is nothing on that drive"): the existing backup on the PC stays
the only copy of stock's `/data`; nothing on USRDATA is to be kept.

- **USRDATA read-only on the Y1** (twentieth boot, `evidence-private/2026-11-05-y1-data-b0/usrdata-superblock.txt`):
  `mmcblk0` read-only; a read-only loop at offset `0x4cf40000` (BusyBox `losetup` has no size limit; PID1 sets
  one): stock ext4, UUID `57f8f4bc-abf4-655f-bf67-946fc0f9f25b` (the backup's), clean, last mounted 1 Aug 2022.
- **eMMC digests before B1** (`emmc-digests-before.txt`, `dd` in 256 KiB blocks, 3.5 min): before USRDATA
  (0–`0x4cf40000`) `00b12562…`, USRDATA `bbcc9190…`, after USRDATA to the end (3875536896 bytes) `71edac3c…`. If
  the outer digests change after B1, the bootloader's own writes (MISC, EXPDB) are checked before anything else.
- **The write guard**: `kernel/y1/emmc-guard.h` (policy) and `kernel/patches-y1/0034-y1-emmc-write-guard.patch`
  (`mtk-sd.c`: checked at the top of `msdc_ops_request`, refused requests completed with `-EPERM` and counted;
  PARTITION_ACCESS tracked, a switch away from the user area counts at once, back only after success; enabled by
  `innioasis,emmc-write-window = <start sectors>` on the eMMC node, absent elsewhere). Writes only inside the
  window, user area, sector addressing, 512-byte blocks, CMD24/25 with an optional CMD23 count (reliable write and
  tag flags); reads unrestricted; erase/trim, write protect, lock, CID/CSD, gen cmd refused; EXT_CSD switches only
  FLUSH_CACHE, CACHE_CTRL, POWER_OFF_NOTIFICATION, HPI_MGMT, ERASE_GROUP_DEF, PARTITION_CONFIG access bits,
  BUS_WIDTH, HS_TIMING (up to HS200), POWER_CLASS. The Y1's eMMC runs MMC high-speed, 8-bit, 50 MHz (no bus test,
  `no-sd`, `no-sdio`). Test `tests/test_y1_emmc_guard.py` (7 cases, every partition boundary, overflow, boot/RPMB,
  CMD23 flags, persistent EXT_CSD fields): OK.
- **The seed image**: `tools/build/y1_data.py` → `out/y1-data-1/Y2DATA-y1.img`, 2 GiB, ext4 `Y2DATA`, UUID
  `79314c69-6e75-4801-8000-000000000102`, `^64bit`, root-owned (debugfs after `mkfs -d`), fixed times;
  contents: `.y2data-schema`, `ssh/authorized_keys.d/authorized_keys` (the owner key), `ssh/host-keys`, `music`,
  `reborn/state/session.json` (screen timeout 0, speaker off), `system/platform` (no power overrides). Checked:
  `e2fsck -fn`, label, UUID, schema, key, ownership. Rebuilt byte for byte: sha256
  `7eed1b14dc409f0042adb754871bdca6809ce9e8d11f5e84200c1b2fff27ada4`.

## 7. B1 — PASS — 2026-11-05

The owner wrote `Y2DATA-y1.img` to USRDATA with mtkclient (no readback file kept). Read on the Y1 afterwards
(`evidence-private/2026-11-05-y1-data-b1/emmc-digests-after.txt`, `mmcblk0` read-only): before USRDATA
`00b12562…` and after it `71edac3c…`, both **unchanged**; USRDATA `7eed1b14…`, **exactly the seed image**. Through a
read-only loop: `LABEL="Y2DATA" UUID="79314c69-…0102" TYPE="ext4"`, `e2fsck -n` clean (23 files).

## 8. B2 build — 2026-11-05

- **PID1** (`initramfs/y1-ninth-boot.h`, `attach_data` under `Y1_TWENTY_FIRST_BOOT`): only if the device tree's
  `innioasis,emmc-write-window` on `mmc@11120000` is exactly USRDATA, `BLKROSET 0` on `mmcblk0` and a writable loop
  (offset `0x4cf40000`, sizelimit `0x80000000`); the boot and RPMB partitions and the card stay read-only. Any
  failure leaves `/data` to the RAM fallback.
- **`initramfs/y1-data/etc/init.d/S03y1-data`**: finds the loop by offset, size, backing file and writability;
  requires `blkid` label `Y2DATA`, the Y1 UUID and ext4; `e2fsck -p` (0/1 continue, anything else not mounted);
  mounts `rw,nosuid,nodev,noatime`; requires schema 1; creates `music`, `reborn`, `system/platform`. Failure: the RAM
  `/data` (`S90y1-reborn data`). Stop: `sync`, unmount (or remount read-only).
- **Device tree** `innioasis-y1-twenty-first-boot.dts`: `innioasis,emmc-write-window = <0x267a00 0x400000>` on
  `&mmc0` switches the guard (overlay 0034) on.
- Build `out/y1-twenty-first-boot-1`, BOOTIMG sha256
  `98dd3c21149572f16e395c96950b155fa3860cfe56f4c1260ab6e23c96457d08` (`0x33b000` bytes).

## 9. B2 run — PASS — 2026-11-05

`evidence-private/2026-11-05-y1-data-b2/run1.txt`, twenty-first boot.

- `mtk-msdc 11120000.mmc: y1 eMMC guard: writes only to sectors 0x267a00..0x667a00`; PID1 attached USRDATA on
  `loop1` (offset 1291059200, size 2147483648, writable) beside Y2ROOT on `loop0` (read-only); `mmcblk0` writable,
  `mmcblk0boot0/1` and the card `mmcblk1` read-only. `S03y1-data`: `Y2DATA: clean`, e2fsck 0, mounted
  `rw,nosuid,nodev,noatime` (1.9 GB). Reborn's library and logs, the power daemon's `battery-ui.json` and the
  dropbear host key are on Y2DATA.
- **Guard**: one CACHE sector (`0x168a00`) read and written back with its own bytes through `mmcblk0`
  (`oflag=direct`): `Input/output error`, the kernel logged `refused CMD24 arg 0x168a00` (four tries by the core),
  the sector unchanged; `/data` unaffected.
- **Persistence**: a marker with the boot ID and the host key fingerprint `SHA256:XrbpLSwR…`, then a restart
  through the power daemon: SSH with strict host-key checking connected without a change, the marker from the
  previous boot was there, `e2fsck` found the filesystem clean (34 files): the shutdown unmounted it.
- Not run: a hard reset during writes (the ext4 journal and `e2fsck -p` cover it; offered to the owner).

## 10. B4 — the built-in card as Internal Storage — 2026-11-14

Owner's request: the card is inside the case and cannot be changed by the owner, so it must not show as an SD card
but as Internal Storage, as the Y2's Y2Media partition does.

- **`S03y1-media`** (`initramfs/y1-data/etc/init.d/`, after `S03y1-data`, before connectivity, MTP and Reborn):
  only a card on MSDC1 (`11130000.mmc`) with a FAT filesystem (its first partition, or the whole card without a
  partition table). It clears PID1's `BLKROSET` on `mmcblk1` and that partition only, mounts it
  `vfat rw,nosuid,nodev,noexec,noatime,fmask=0077,dmask=0077,utf8,shortname=mixed,errors=remount-ro` at
  `/media/internal`, creates `music` (FAT ignores case: stock's `Music` folder is that folder) and writes
  `/run/y2/internal-media.json` as the Y2's platform does for Y2Media: `Ready` with this boot's ID, the mount ID
  and the device ID. Reborn's `internal_music_directory()` then takes `/media/internal/music` as Internal Storage
  with no Reborn change. On any failure the record says `Unavailable` with a reason and Internal Storage stays
  `/data/music`; stop syncs and unmounts (read-only remount if busy).
- **MTP** (`S45y1-mtp`): the one storage is **Internal Storage** = `/media/internal/music` when the record names
  this boot and the current mount, else **Music** = `/data/music` as before.
- **Radio firmware** (`S41y2-connectivity`): the Firmware folder it judges follows the storage MTP shows; the
  platform's import already looks in `/media/internal/music/Firmware` when Internal Storage is ready.
- **Platform** (`y2p-core/src/volume.rs`, in Y2ROOT): on the Y1, `/media/internal` is Ready on MSDC1 with no fixed
  UUID (the owner's FAT serial), so Settings → Storage shows the card's size and free space instead of Y2DATA's.
- **Not done**: no FAT check at boot (a 116 GiB check on every start; `errors=remount-ro` instead). Music already in
  `/data/music` is not moved; it leaves the library while the card is Internal Storage, and stays on Y2DATA.
- **Tests**: `tests/test_y1_media.py` (order, install, the record, refusal of another controller or filesystem, MTP's
  choice); the Y1 volume test covers the card. To run on the player: the kernel log's `y1-media: … mounted at
  /media/internal as Internal Storage`, MTP's "Internal Storage", Settings → Storage with the card's size, a copied
  song scanned and played after a reboot.
