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

## 7. M1 run 1 and the retry — 2026-11-05/06

**Run 1** (`out/y1-twenty-second-boot-1`): the owner reported that the player was not charging; the battery ran
flat, the Y1 then looped on the power daemon's voltage guard and was recovered with a power bank and the nineteenth
image ([charge-mode plan](Y1-CHARGE-MODE-PLAN.md) §1). No log survived: the gadget's messages went to the screen
only. INFERRED: the PC never configured the new gadget, so the Y1 charged at the 70 mA floor, below its load.

**Retry** (the owner's choice after C1 passed, battery well charged): `out/y1-twenty-fourth-boot-1`, the twenty-third
system (charge-only mode, Y2DATA, the daemon) with the twenty-second's configfs kernel and PID1 gadget; the gadget's
steps and refusals also go to the kernel log (`y1 usb gadget: …`). BOOTIMG sha256
`b06dafcdb8ee5d903df08d7ab04436e52e947bd3ee8f968dc7a3dfb94f63dfe4` (`0x340800` bytes). Recovery if USB does not come up:
the nineteenth or twenty-third image with mtkclient.

**M1 retry — PASS — 2026-11-06** (`evidence-private/2026-11-06-y1-mtp-m1/run2.txt`): `configfs-gadget.y1` with the fixed
MACs, `y1 usb gadget: ECM + ACM bound to musb-hdrc.3.auto` at 3.6 s; UDC `configured`; the PC lists `0525:a4aa`;
`usb0` and SSH work; `/dev/ttyGS0` present; `y1-usb-input` online, the charger in CC at the 450 mA limit, no fault.
Run 1's failure is not reproduced; with no log from it, the likeliest cause is the battery already too low at boot
(INFERRED).

## 8. M2 — 2026-11-06

`initramfs/y1-mtp/etc/init.d/S45y1-mtp` (after `S40network`, before Reborn; stops before `/data` is unmounted):
only with the real Y2DATA at `/data` (the loop at USRDATA's offset, schema 1); unbinds PID1's gadget, adds
`ffs.mtp` as the first function (then `ecm.usb0`, `acm.GS0`), mounts FunctionFS at `/dev/ffs-mtp`, writes
`/run/y1-mtp/umtprd.conf` (the Y2's format, product "Y1", one storage `"/data/music" "Music" "rw,notmounted"`),
starts `umtprd` under a supervisor shell (at most five starts, `ulimit -v 65536`), waits for its endpoints and
`ffs.mtp/ready`, binds again, mounts the storage (`umtprd -cmd:mount:Music`, acknowledged in
`/run/y2/mtp-roots.json`), and writes `/run/y2/usb-transfer.json` (`org.y2linux.usb-transfer/v1`) every second
from `mtp-events.json` and the UDC state, raising `refresh_generation` when umtprd's generation moves on or the
computer lets go. Stop: unbind, stop umtprd, unmount FunctionFS, relink ECM + ACM and bind again.

**Live run** on the twenty-fourth boot (the script started over SSH; `m2-live.txt`): `MTP on musb-hdrc.3.auto: Music =
/data/music`; the PC sees five interfaces, **MTP** (class 6) first, then ECM and ACM, so the musb's extra endpoints
work with all three functions; `mtp-roots.json` `{"pid":226,"roots":["/data/music"]}`; the receipt written; UDC
`configured`; SSH survived the rebind. Image: `out/y1-twenty-fifth-boot-1` (PID1 installs `S45y1-mtp` under
`Y1_TWENTY_FIFTH_BOOT`, the service list gains it), BOOTIMG sha256
`9757251c79418167975124c9f2d247ec2b92af2275eebb0b533318d423fa5ff5` (`0x341000` bytes).

## 9. M2 uploads fail above one USB read — open (2026-11-06)

The owner's file manager showed the Y1 and created folders, but copying files failed. Reproduced from the PC with
a minimal PTP/MTP client over libusb (a scratch script, not kept): `OpenSession`, storage "Music" (rw, 2.04 GB,
1.92 GB free), listing, `SendObjectInfo` all OK; `SendObject` succeeds for 100 and 40 000 bytes (files on Y2DATA)
and fails (`0x2007` Incomplete Transfer, or a stall) for every size larger than one `umtprd` read: with the Y2's
`usb_max_rd_buffer_size 0x10000`, 66 000 bytes stall after `.part` reached 65 524 bytes (the first read minus the
12-byte header); with `0x80000`, 66 000 bytes succeed and 529 244 bytes fail. An aborted upload leaves the rest of
its data in the endpoint, which `umtprd` then reads as commands (`0x2005`, odd codes): each test needs a fresh
`umtprd` and a USB reset from the PC.

- The recorded `error` 2 (ENOENT) is stale: the patched `SendObject` abort path reports whatever `errno` holds
  (`buildroot/package/y2-mtp/0001-…patch`, `failure = errno ? errno : EIO`); the trace shows a failed
  `/etc/localtime` open just before.
- INFERRED cause: the Y1's musb gadget in PIO mode (`MUSB_PIO_ONLY`) loses or never starts the OUT request that
  follows a request filled exactly with full packets; the Y2 uses the same umtprd on 6.18 with DMA. A suspected race
  (a request queued from the second CPU while `musb_g_giveback` holds `busy`, not restarted by the PIO path of
  `rxstate`) is not confirmed: pinning umtprd to CPU 0 (a freestanding `sched_setaffinity` helper,
  `out/y1-pin/pin.c`) gave an inconclusive run (the session timed out before the upload).
- Next: a debug kernel (musb RX path logging, or one CPU) to settle it, then a fix in a Y1 overlay of
  `musb_gadget.c`. Until then music goes over `scp` (19 MB in about 6 s); the twenty-fifth image (MTP at boot) is
  not for use.

## 10. X1 — the tracing image — 2026-11-06

Owner-approved (diagnosis only). `kernel/config/y1-twenty-sixth-boot.config`: the twenty-fourth fragment plus
`FTRACE` and `ENABLE_DEFAULT_TRACERS` (resolved: `TRACING`, `EVENT_TRACING`, `TRACEPOINTS`), for the musb trace
events (`musb_req_enq`, `_start`, `_rx`, `_gb`, `musb_log` with the driver's `musb_dbg` lines, `musb_isr`); PID1,
tree and initramfs as the twenty-fourth. Build `out/y1-twenty-sixth-boot-1`, BOOTIMG sha256
`932add281f3de431318a2d26ccce98ecd2372293efd1f3fb3d73e9416a45ff5f` (`0x392800` bytes). Plan: MTP started by hand,
the events on, one 66 000-byte upload from the PC, the trace read over SSH.

**X1 result — the cause, CONFIRMED — 2026-11-06** (`evidence-private/2026-11-06-y1-mtp-x1/`):

- `trace1.txt` (request events, one 66 000-byte upload): the 64 KiB OUT request on `ep1` completed with **65 500**
  bytes: 127 full packets (each step +512) and the final 476, 128 RX interrupts for 128 + 1 packets sent: one full
  packet never reached the request; the reply `0x2005` (umtprd read mid-data as a command) fits the data's first
  packet being the lost one.
- `trace2.txt` (with register access): `umtprd`'s thread queues the next OUT request on CPU 1;
  `musb_ep_restart` reads `RXCSR` = `0x0003` (**a packet already waiting**) and writes `0x0057`, i.e.
  `MUSB_RXCSR_FLUSHFIFO`: the waiting packet is discarded, and the session stalls.
- Mainline 6.18 `musb_ep_restart()` (`drivers/usb/musb/musb_gadget.c:1163`) flushes the RX FIFO unconditionally
  whenever a request is queued on an idle OUT endpoint. On the Y1 (PIO, the reader on the second core, a few µs
  behind the packets) that drops a packet whenever one arrives before the next read is queued: commands, data
  headers, mid-transfer packets. ECM rarely hits it (its OUT requests are usually queued in advance).
- **Proposed fix (X2)**: a `patches-y1` overlay of `musb_gadget.c`: when `RXCSR` shows `RXPKTRDY` at restart, read
  that packet into the new request (`rxstate`, as the RX interrupt would) instead of flushing; flush only an empty
  FIFO, as now. Called under the musb lock as before.

## 11. X2 build — 2026-11-06

Owner-approved. `kernel/patches-y1/0035-musb-rx-restart-keeps-waiting-packet.patch`: in `musb_ep_restart()`, an OUT
endpoint whose `RXCSR` shows `RXPKTRDY` when a request is queued is served by `rxstate()` (as the RX interrupt would),
not flushed; an empty FIFO is flushed as before (manifest hashes added). Image `out/y1-twenty-seventh-boot-1`: the
twenty-fifth (MTP at boot) with the fix, BOOTIMG sha256
`e59ea7f21db119e48466c687a2412c5d4b31092365ecfb0accbcbe7eb29605a8` (`0x341000` bytes).

## 12. X2 run — PASS (client) — 2026-11-06

`evidence-private/2026-11-06-y1-mtp-x2/run1.txt`, twenty-seventh boot: PID1's gadget at 3.9 s, `S45y1-mtp` at 16.3 s
(`Music = /data/music`), the charger in CC at 450 mA. From the PC (the minimal client): uploads of 66 000 and
529 244 bytes **OK** (both failed before), a 9 MB song **OK in 2.3 s (about 3.9 MB/s)**, the download of it OK;
SHA-256 identical on the PC, on the Y1 and after the round trip; three deletes OK; the receipt's
`refresh_generation` moved to 3 and Reborn rescanned by itself (3 tracks with the uploaded copy, 2 after the
deletes). SSH and the serial shell (`sh` on `ttyGS0`, from `getty -n`) unaffected. **MTP uploads: fixed.** Open:
the owner's file manager (M3/M4: cancel, unplug mid-copy, delete while playing, disk full).

**Why the Y2 is not affected** (checked 2026-11-06): it was. The Y2 found the same mainline bug and fixed it on
2026-10-05 (`ac34f82f`, `kernel/patches/0065-y2-musb-pio-progress.patch`: `musb_ep_start()` serves a freshly
queued OUT request from a packet already in the FIFO instead of flushing it, the "late_pio" path; a restart after
dequeue still flushes). The Y1 overlay set started with none of the Y2's patches and uses mainline's MediaTek glue,
so the fix was not there. The two fixes are equivalent for this case (0035 flushes only an empty FIFO and writes
nothing while a packet waits). The earlier explanation "the Y2 avoids it through DMA" was an unchecked inference
and wrong.

**0035 aligned with the Y2** (the owner's choice, 2026-11-06): `kernel/patches-y1/0035-musb-ep-start.patch` replaces
the first form (`…-rx-restart-keeps-waiting-packet`, build 1): the Y2's `musb_ep_start()` (from its patch 0065,
without the Y2's RX event counters), called from `musb_ep_restart_resume_work` for a freshly queued request:
TX → `txstate`, RX → `rxstate` (serves a waiting packet, never flushes); `musb_ep_restart()` with its flush stays
for the restarts after a halt (`musb_gadget_ep0.c`, `musb_gadget.c` set-halt), as on the Y2. Build
`out/y1-twenty-seventh-boot-2`, BOOTIMG sha256 `e9b57b6de17949e7b9125cf32d11be013e0e011dfc6ec0798410da5eac9d8eb8`.
