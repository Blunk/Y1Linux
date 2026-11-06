# Y1 music transfer over MTP (milestone B3)

Status: **planned** (2026-11-05), not admitted. The owner asked for MTP, as on the Y2, after Y2DATA became
persistent ([data plan](Y1-DATA-PLAN.md), B2 PASS). Each stage has its own admission.

## 1. Evidence

Read-only (agent notes `out/y1-mtp-notes.md`, file:line there). CONFIRMED unless marked.

- **The Y2's gadget** (`platform/crates/y2-platform/src/commands/service/usb/gadget.rs`, `usb.rs`): configfs
  `usb_gadget/y2`, VID/PID `0x0525:0xa4aa`, strings Y2Linux/Y2, one configuration (MaxPower 500), in MTP mode
  `ffs.mtp` + `ecm.usb0` with Windows OS descriptors; order: unbind, functions, FunctionFS at `/dev/ffs-mtp`,
  `/run/y2/umtprd.conf` generated, `umtprd` started, wait for its endpoints, bind the UDC. Storages are declared
  "notmounted" and added at runtime (`umtprd -cmd:mount:…`). Reborn rescans when `refresh_generation` in
  `/run/y2/usb-transfer.json` changes.
- **It cannot be reused as is**: the gadget lives in the platform's service daemon, which also runs SD mounts
  read-write, update checks and RECOVERY/MISC writes; it stays off on the Y1.
- **`umtprd` is already on the Y1** (baseline-220 rootfs: v1.9.1 with the Y2's patches): uploads go to a hidden
  `.part` file, fsync, then rename without replacing, so an unplugged copy never appears and a playing file is never
  overwritten. It only serves paths below roots it pinned at start; a storage declared "notmounted" and mounted at
  runtime works for any path. It exits if its parent is PID 1 (needs a supervisor shell as parent). It uses POSIX
  message queues.
- **The Y1 kernel** uses the legacy `g_cdc` (ECM + ACM); `USB_CONFIGFS`, FunctionFS and `POSIX_MQUEUE` are off.
  PID1 relies on `ttyGS0` (its rescue shell until `switch_root`; inittab's getty after).
- **Endpoints**: the MT6572 musb (mainline `mediatek.c`) has 7 endpoint pairs; MTP + ECM + ACM needs 6 IN, 3 OUT.
  `g_cdc` only ever used 4 IN / 2 OUT; endpoints 5–7 are untested on the Y1 (INFERRED risk). PIO only: slower than
  the Y2 (INFERRED).

## 2. Design

- **Kernel**: `g_cdc` off; `USB_CONFIGFS` with `F_FS`, `ECM`, `ACM`; `POSIX_MQUEUE`.
- **PID1**: builds the configfs gadget itself (ACM + ECM, the Y2's identity, MaxPower 500, so the Y1's charging
  rule still sees a configured 500 mA PC) and binds it before its USB shell; the rescue shell and SSH keep working.
- **`S45y1-mtp`** (new, after `S03y1-data` and `S20y2-usb`, before Reborn), only when `/data` is the real Y2DATA:
  unbind, add `ffs.mtp`, mount FunctionFS, start `umtprd` under a small supervisor loop (5 restarts, address-space
  limit as on the Y2) with one storage **"Music" = `/data/music`, read-write**, declared "notmounted" and mounted
  once it is running, rebind; turn umtprd's events into `/run/y2/usb-transfer.json` so Reborn rescans. Stop: unbind,
  stop `umtprd`, unmount FunctionFS, before `/data` is unmounted. The 116 GiB card is not exposed.
- If the extra endpoints fail, MTP + ECM without ACM (the Y2's own combination).

## 3. Stages

| Stage | What |
| --- | --- |
| **M1** | Kernel and PID1 gadget only (ACM + ECM through configfs): serial shell, SSH, 450 mA charging, power-off unchanged |
| **M2** | MTP added: the PC sees the Y1 as a media player with a "Music" folder; browse, copy to and from it |
| **M3** | Edge cases: cancel and unplug mid-copy, delete while playing, disk full, Reborn's rescan, files persist, `e2fsck` clean |
| **M4** | Host coverage the owner uses (Linux gvfs/KDE; Windows native MTP; macOS needs an MTP app) |

## 4. Risks

The untested musb endpoints (fallback without ACM); losing PID1's rescue shell if its gadget step fails (the
console text and a reflash remain); a busy `/data` at shutdown (MTP stops first); Y2DATA filling up (1.8 GB free);
slow transfers (PIO); the development VID/PID on Windows.

## 5. Admission

| Stage | Item | Bound |
| --- | --- | --- |
| M1 | **configfs USB gadget** | ACM + ECM as today, built by PID1; no MTP; no storage change |
| M2–M3 | **MTP** | one storage, `/data/music` on Y2DATA, read-write through umtprd's safe-write path; nothing else exposed |
| M4 | **Host checks** | the owner's computers |

## 6. M1 build — 2026-11-05

Owner-approved (M1 and M2). `kernel/config/y1-twenty-second-boot.config`: the twenty-first fragment without
`USB_CDC_COMPOSITE`, with `USB_CONFIGFS`, `_ACM`, `_ECM`, `_F_FS` and `POSIX_MQUEUE` (resolved: libcomposite,
u_serial, u_ether, f_fs). PID1 (`initramfs/y1-twenty-second-boot.h`, `Y1_TWENTY_SECOND_BOOT`): after finding the
UDC it mounts configfs and builds `usb_gadget/y1`: 0525:a4aa, "Y2Linux"/"Y1"/"Y1LINUX-DEV-01", `c.1` MaxPower 500,
bmAttributes 0x80, `ecm.usb0` (fixed MACs 02:59:31:00:00:01 device, :02 host) and `acm.GS0`, bound to the UDC; one
screen line reports it. Device tree and initramfs list unchanged apart from names. Build
`out/y1-twenty-second-boot-1`, BOOTIMG sha256 `960bd8a7c2703882e9cadd1206fbf0cacef573b2bd73ff85a6634da4886042f5`.
