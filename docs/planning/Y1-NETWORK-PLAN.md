# Y1 USB networking and SSH (system milestone C)

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The step builds named
> below (the *N*th boot, its variant, tree, fragment and PID1 header) are the record of the test images that ran
> on the player; their files are folded into that build.

Status: **PASS** on the owner's player (2026-11-05, [run](#8-run--pass--2026-11-05)): USB networking and key-only
SSH at the Y2's `10.42.0.1`; approved by the owner with all four admission items the same day. Follows the
[system plan](Y1-SYSTEM-PLAN.md) (milestone A PASS: the Y2's init and a Y1-safe service set; a shell on
the USB serial port).

## 1. Goal

The Y1 appears on the PC as a USB network interface in addition to the serial port, at the Y2's address
`10.42.0.1`, and accepts SSH logins for root with a key, as the Y2 does (`docs/INSTALL.md`: the PC takes
`10.42.0.2/24`, `ssh root@10.42.0.1`). File copies (`scp`, `ssh … cat >`) then replace the serial helper
scripts.

Not in scope: persistent host keys and settings (milestone B; until then they live in RAM), MTP, Wi-Fi,
IPv6, a route to the internet, NTP, the Y2's configfs gadget service (`S42y2-readiness` stays off).

## 2. Evidence

- **The Y2:** `S20y2-usb` waits for `usb0`, then `ip address replace 10.42.0.1/24 dev usb0` and `up`; it is
  safe on the Y1 (no hardware access). `S50dropbear` runs the Y2's `ssh-daemon`, which needs the `Y2DATA`
  schema and keeps host keys and the authorised key on `/data` (system review, `p/devices/ssh.rs`). The
  image has `/usr/sbin/dropbear`, `dropbearkey`, `/sbin/ip`.
- **Kernel:** `USB_CDC_COMPOSITE` (legacy `g_cdc`: one configuration with CDC ECM and CDC ACM) needs
  `NET` only; the serial port stays `ttyGS0`. Its configuration is self-powered (`cdc2.c:133`), like
  `g_serial`'s, so the Y1 charger's budget source (configured = host bus reset and a 0 mA or ≥ 500 mA
  budget) is unchanged. It needs four bulk and two interrupt endpoints; the MUSB FIFO table has eight
  endpoints.
- **Key:** no Y2 SSH key exists on the owner's PC (`~/.ssh/y2linux_ed25519` absent).

## 3. Design

- **Kernel (eleventh boot):** the tenth fragment with `USB_CDC_COMPOSITE` instead of `USB_G_SERIAL`, plus
  `INET` (IPv4 only; no IPv6, no forwarding, no netfilter).
- **Key:** a key pair made for the Y1 in `out/y1-ssh/` (not committed, not in `~/.ssh`): the private half
  stays there; the public half goes into the initramfs override as `/root/.ssh/authorized_keys`.
- **Services:** the Y1 list gains `S40network` (brings up `lo`, as on the Y2) and `S20y2-usb` (the Y2's
  address on `usb0`), and a Y1 script `S50y1-dropbear` instead of the Y2's `S50dropbear`: `dropbear -R -s
  -g -p 22` (host keys created in RAM at first login, password logins off, root only by key). The serial
  getty stays.
- **PC side (owner):** `sudo ip address add 10.42.0.2/24 dev <interface>` on the new interface (as in the
  Y2's install guide), then `ssh -i out/y1-ssh/id_ed25519 root@10.42.0.1`.

## 4. Later

Milestone B moves the host keys and `authorized_keys` to `Y2DATA`, so the host key survives a reboot and
the Y2's own `ssh-daemon` can take over.

## 5. Test

The PC lists `cdc_ether` and `cdc_acm` for the Y1; the serial shell still works; `ping 10.42.0.1`;
`ssh root@10.42.0.1 uname -a`; `scp` a file to `/tmp` and back; charging still reports the 450 mA PC
current; password login is refused.

## 6. Admission

| Item | Bound |
| --- | --- |
| **Ethernet over USB** (`g_cdc`: ECM + ACM) instead of the serial-only gadget | USB device mode as before; the PC gets a network interface; no host mode |
| **IPv4 networking in the kernel** (`INET`) | one link, `usb0` at `10.42.0.1/24`, and loopback; no IPv6, no forwarding, no Wi-Fi |
| **An SSH server** (`dropbear`) | root login by the Y1 key only, no passwords; host keys in RAM until milestone B; reachable only over the USB cable |
| **A key pair for the Y1** | made in `out/y1-ssh/`, not committed; only the public half in the boot image |

## 7. Build — 2026-11-05

- **Key:** `out/y1-ssh/id_ed25519` (ed25519, comment `y1linux-dev`, git-ignored); its public half is
  built into the initramfs as `/y1-system/data/ssh/authorized_keys.d/authorized_keys` (0600).
- **Image paths:** the Y2 image links `/etc/dropbear` to `/data/ssh/host-keys` and `/root/.ssh` to
  `/data/ssh/authorized_keys.d`; PID1 creates those directories (0700) and the key in the RAM overlay's
  upper layer, so the Y2's layout holds and a later `Y2DATA` on `/data` takes over.
- **Services** (`initramfs/y1-network/etc/init.d/`): the list gains `S20y2-usb` and `S40network` (the
  Y2's) and `S50y1-dropbear` (`dropbear -F -R -s -g -p 22` under `start-stop-daemon`).
- **Kernel** `y1-eleventh-boot.config`: the tenth fragment with `USB_CDC_COMPOSITE` instead of
  `USB_G_SERIAL`, `INET`, no `IPV6`; initramfs list `initramfs/y1-eleventh-boot.list`. Tree: the tenth,
  new model string.

Build `out/y1-eleventh-boot-1`, BOOTIMG sha256
`d1acd16bc8f984c7d6c591c85def91eb271df63c9f87c67ba28650ce2bc04a94` (`0x2e3000` bytes). Physical status
NOT_RUN.

## 8. Run — PASS — 2026-11-05

Build `d1acd16b…`; `evidence-private/2026-11-05-y1-network/ssh-1.txt`. The PC named the new interface
`usb0`; the owner gave it `10.42.0.2/24`. `ping 10.42.0.1`: 0 % loss, 0.21 ms. SSH with
`out/y1-ssh/id_ed25519` (a separate `known_hosts` in `out/y1-ssh/`, since the host key is new at every boot
until milestone B): `Linux y2linux 6.18.56-y1-eleventh-boot … armv7l`, `usb0 10.42.0.1/24`, `lo`;
running `init`, `syslogd`, `crond`, `dropbear` and the shells. A 1 MiB random file copied to `/tmp` with
`scp` and back is identical. A password login is refused (`Permission denied (publickey)`). Charging
continues through the CDC composite gadget: `phase=CONSTANT_CURRENT`, `input_budget_ua=500000`, no fault;
the limit was 200 mA because the battery (4.116 V) was in the constant-voltage phase and the charger's
finish stage steps down (`finish=200000`), PMIC die 40 °C, no thermal cap.

**Milestone C: PASS.**
