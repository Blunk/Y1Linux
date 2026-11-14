# Installing Y2Linux on the Innioasis Y1

The Y1 port runs the Y2's userspace (Y2ROOT, the platform and Reborn) on the Y1's MT6572 with its own kernel. It
is **in development**. You build the images yourself (or take them from a
[Y1Linux release](https://github.com/Blunk/Y1Linux/releases)) and write them **once** with **mtkclient** through
the MediaTek preloader; the stock Y1 has no SP Flash Tool package for Y2Linux and no ADB. After that the player
updates itself over Wi-Fi, through the Y1 recovery image in RECOVERY ([Updating](#updating)).

Read the whole page once before you start. The background for each step is in the Y1 plans under
[`docs/planning/`](planning/) (`Y1-*.md`).

## What you need

- A Linux computer and a USB cable.
- **mtkclient** 2.1.4.1 ([bkerler/mtkclient](https://github.com/bkerler/mtkclient)). It talks to the Y1's preloader
  and loads its bundled legacy DA into RAM; the Y1 has SBC, SLA and DAA off, so no auth file is needed.
- A **Type A** Y1 (the wheel works on firmware 2.0.0 and newer). The port was brought up on stock 3.0.2; Type B
  players have a different wheel controller and are not supported.
- The build environment of [BUILDING](BUILDING.md) (about 15 GB free disk for the root filesystem).
- For Bluetooth: the **Innioasis Y1 firmware package 3.0.2** (`rom.zip`, SHA-256 `7ee9aa5b…f764b1`), from
  Innioasis or the [y1-community stock ROM release 3.0.2](https://github.com/y1-community/y1-stock-rom/releases).
- For Wi-Fi as well: the **Innioasis Y2 firmware package v3.2.0** (`y2_v3.2.0_FM-20260813`), from Innioasis, or the
  same build as `rom_y2.zip` from the
  [y1-community release 3.2.0-fm](https://github.com/y1-community/y1-stock-rom/releases/tag/3.2.0-fm). Yes, the
  Y2's: see below.

## Radio firmware: which files, from where, stored where

Bluetooth and Wi-Fi need MediaTek firmware that may not be redistributed, so **no Y2Linux image contains it**. The
player reads it out of the stock `system.img` you copy onto it, checks every file against its SHA-256 (pinned in
`initramfs/y1-bluetooth/etc/y2linux/radio-firmware.json`) and keeps only these files:

| File on the player | Taken from | Bytes | SHA-256 | Needed for |
| --- | --- | --- | --- | --- |
| `WMT_SOC.cfg` | Y1 3.0.2 `system.img`: `/etc/firmware/WMT_SOC.cfg` | 82 | `161f18b6…f1e0` | Bluetooth, Wi-Fi |
| `mt6572_82_patch_e1_0_hdr.bin` | Y1 3.0.2 `system.img`: `/etc/firmware/mt6572_82_patch_e1_0_hdr.bin` | 39 868 | `5c13ebad…bfc1` | Bluetooth, Wi-Fi |
| `BT.defaults` | cut from Y1 3.0.2 `system.img`: `/lib/libbluetooth_mtk.so` (offset `0x3004`) | 32 | `a317b67f…119c` | Bluetooth |
| `WIFI.defaults` | cut from Y1 3.0.2 `system.img`: `/lib/libcustom_nvram.so` (offset `0x3cf4`) | 512 | `9cfed6e4…a8b4` | Wi-Fi |
| `WIFI_RAM_CODE_MT6582` | **Y2** v3.2.0 `system.img`: `/etc/firmware/WIFI_RAM_CODE_MT6582` | 160 368 | `4def3752…9b82` | Wi-Fi |

All of them are stored in **`/data/firmware/mt6572/`** on the Y2DATA partition (USRDATA), root-only. The player
also writes its own radio records next to them in `/data/connectivity/` (`bt-record`, `wifi-record`) with a random,
locally administered address for each radio, created once and then kept.

### Why the Innioasis package?

Innioasis built the Y1 **without Wi-Fi**: its firmware has no Wi-Fi driver and no Wi-Fi firmware at all. The radio
chip inside the MT6572 does have a Wi-Fi core, and MediaTek's MT6572 driver loads a file named
`WIFI_RAM_CODE_MT6582`. The Y2's copy of that file is byte-identical to the one MT6572 phones ship, so the Y1 uses
the Y2 package's file. Bluetooth uses the Y1's own patch and configuration. Neither package's calibration is used:
the Y1 has no factory Wi-Fi calibration, so Wi-Fi runs on the Y1 library's defaults.

**Only these versions work:** a `system.img` from another Y1 or Y2 version is refused, because the hashes do not
match. Bluetooth works without the Y2 package; Wi-Fi needs both.

## What the install changes

| Partition (stock name) | Written with | Holds afterwards |
| --- | --- | --- |
| BOOTIMG | `BOOTIMG.img` (Y1 build) | the Y1 kernel and its init, the Y1's `/etc/y2linux` overrides |
| ANDROID (`/system`) | `rootfs.ext4` (512 MiB of the 650 MiB partition) | Y2ROOT, the Y2's userspace, read-only |
| RECOVERY | `RECOVERY.img` (Y1 build), once | the Y1 recovery image, which installs updates |
| USRDATA (`/data`, 2 GiB) | `Y2DATA-y1.img` | Y2DATA: settings, library, music, SSH keys, the radio firmware |
| MISC | by the player itself | the update request (Android's `bootloader_message`) |
| Preloader, MBR/EBR1, LK, NVRAM, PROTECT_F/S, SECCFG, FAT, the built-in microSD | **never written** | stock |

Stock Android and its user data are erased; stock no longer starts. The Y1 kernel has an eMMC write guard: the
normal system can write only USRDATA, RECOVERY and MISC; only the recovery image writes BOOTIMG and ANDROID, and
neither writes the image it runs from ([Recovery and updates](RECOVERY-AND-UPDATES.md#on-the-y1)). Music lives in
`/data/music` on Y2DATA (about 1.9 GB); the stock FAT partition and the built-in microSD card are not used.

## Back up first

Before any write, read the whole eMMC with mtkclient and keep the files somewhere safe. They hold your player's
calibration (NVRAM, PROTECT_F/S) and are your only way back to stock:

```sh
python mtk.py printgpt
python mtk.py rf y1-emmc-full.bin              # the whole user area, about 3.6 GB
python mtk.py r BOOTIMG,RECOVERY,ANDROID,USRDATA boot-stock.img,recovery-stock.img,android-stock.img,usrdata-stock.img
sha256sum *.bin *.img > SHA256SUMS
```

mtkclient waits for the player: connect the **switched-off** Y1 by USB (hold the centre button to switch it off).

## Build the images

From the repository root on the `y1` branch:

1. **An SSH key** for the player (the data image authorises it; there is no password login):

   ```sh
   mkdir -p out/y1-ssh && ssh-keygen -t ed25519 -N '' -f out/y1-ssh/id_ed25519
   ```

2. **Y2ROOT** from this branch (the firmware import from two packages needs this branch's platform; the Y2's
   release root images do not have it):

   ```sh
   python3 tools/production/build.py --output out/y1-root --public
   ```

   Result: `out/y1-root/buildroot/images/rootfs.ext4` (512 MiB, label `Y2ROOT`).

3. **BOOTIMG** (the Y1 kernel with Bluetooth, Wi-Fi, MTP, charging and Reborn's devices; `out/y1-boot` must not
   exist yet). Its initramfs carries the updater from step 2's build (`y1-update-core`, the recovery screen and
   their libraries), so step 2 comes first:

   ```sh
   python3 tools/build/run.py --manifest kernel/patches-y1/manifest.json --output out/y1-boot \
       -- sh /project/tools/build/y1-build.sh /project/out/y1-root
   ```

   Result: `out/y1-boot/BOOTIMG.img` and the recovery image `out/y1-boot/RECOVERY.img` (the same kernel, which
   installs updates) with their `SHA256SUMS`.

4. **Y2DATA** (2 GiB, with your key from step 1):

   ```sh
   python3 -m tools.build.y1_data out/y1-data
   ```

   Result: `out/y1-data/Y2DATA-y1.img` and `y1-data.json` with its SHA-256.

## Write the images

Connect the switched-off Y1 for each command (mtkclient waits for it), from the mtkclient folder:

```sh
python mtk.py w USRDATA <repo>/out/y1-data/Y2DATA-y1.img
python mtk.py w ANDROID <repo>/out/y1-root/buildroot/images/rootfs.ext4
python mtk.py w BOOTIMG <repo>/out/y1-boot/BOOTIMG.img
python mtk.py w RECOVERY <repo>/out/y1-boot/RECOVERY.img
```

From a [release](https://github.com/Blunk/Y1Linux/releases): check the files with `sha256sum -c SHA256SUMS`,
`gunzip rootfs.ext4.gz`, and write `rootfs.ext4`, `BOOTIMG.img` and `RECOVERY.img` the same way; build only
Y2DATA (step 4) yourself.

`w` finds the partition by name and stops if it is absent. Never write PRELOADER, MBR, EBR1, UBOOT, NVRAM,
PROTECT_F, PROTECT_S or SECCFG. To check a write, read the partition back (`python mtk.py r BOOTIMG readback.img`)
and compare the hash over the image's length.

RECOVERY is written **once**: the normal system does not need it, only updates and the way back do, and every
update brings its own recovery image and writes it there itself.

## First start

Hold the centre button for about 5 s to switch the player on. The first start creates the data folders, then
Reborn starts and music works. Bluetooth and Wi-Fi wait for the radio firmware.

**Install the radio firmware over USB:**

1. Connect the Y1 to the computer. It appears as a media player (MTP) with one storage, **Internal Storage** (the
   built-in microSD; **Music** on Y2DATA if the card cannot be mounted). Use its folder **Firmware** (create it on
   Internal Storage if it is not there; on Music it comes with a `README.txt`).
2. Unpack the Y1 3.0.2 `rom.zip` and **copy its `system.img`** (raw ext4, about 650 MB) into `Firmware` on that storage.
   Copy, don't move: the player deletes its copy afterwards.
3. Within about 10 s the player checks the file, installs the four Y1 files, deletes the copy and starts
   Bluetooth.
4. For Wi-Fi: unpack the Y2 v3.2.0 package and copy **its `system.img`** into `Firmware` the same way (about
   520 MB from Innioasis's zip, about 860 MB from the community `rom_y2.zip`; it needs that much free space on
   that storage). The player keeps only `WIFI_RAM_CODE_MT6582`, deletes the copy and registers Wi-Fi without a restart.
   The `Firmware` folder disappears once both are installed.

The two copies can go in either order. Reborn's firmware screen still names the Y2 package and a "Firmware
drive"; on the Y1 the folder is `Firmware` on the MTP storage (on Music its `README.txt` names both packages).

Then switch Wi-Fi on in Reborn and pick your network, or pair a Bluetooth headset. Switch the player off with a
3 s hold of the centre button.

## Developers: SSH over USB

With the player running and plugged in, it also shows up as a USB Ethernet adapter. The device is `10.42.0.1`;
give your computer's new interface `10.42.0.2/24` with no gateway:

```sh
ip -br link                                    # find the new interface (e.g. enp…u2i1 or usb0)
sudo ip address add 10.42.0.2/24 dev <interface>
ssh -i out/y1-ssh/id_ed25519 -o UserKnownHostsFile=out/y1-ssh/known_hosts root@10.42.0.1
```

Useful commands on the device:

```sh
y2-platform firmware status                          # radio firmware: Ready, Missing, Installing or Failed
y2-platform firmware install /path/to/system.img     # the same as the Firmware folder, either package
ls -l /data/firmware/mt6572 /data/connectivity
cat /sys/devices/platform/18070000.connectivity/status
```

Instead of the import you can stage the files on the computer and copy them with `scp`:
`tools/connectivity/provision.py --profile y1` checks the files you extracted from both `system.img`s against the
[firmware inventory](knowledge/evidence/y1-firmware-inventory.json) and cuts `WIFI.defaults`
(procedure: [Y1 Wi-Fi plan §9](planning/Y1-WIFI-PLAN.md#9-w3-build--2026-11-07)). Copy the result to
`/data/firmware/mt6572/` (root, mode 0600) and restart the connectivity script
(`/etc/init.d/S41y2-connectivity restart`).

## Updating

**Over Wi-Fi** (once RECOVERY holds the Y1 recovery image): the player checks
[Blunk/Y1Linux](https://github.com/Blunk/Y1Linux/releases) once a day on Wi-Fi. Settings → System → Software Update →
**Update Now** needs the charger or at least half a battery. The player downloads and checks the signed update,
restarts into the recovery image, which shows *Installing Update*, backs up the running version, installs the new
one and restarts by itself. The download is about 60 MB (minutes on the Y1's slow Wi-Fi); the install about two
minutes. Music, settings, library, Wi-Fi networks, pairings and the radio firmware stay. If the new version does not start properly within three minutes, the next start brings the
previous one back by itself. **Restore Previous System** (Settings → System → About, tap Version seven times, then
Diagnostics) goes back once by hand. How it works: [Recovery and updates](RECOVERY-AND-UPDATES.md#on-the-y1).

**First update on a player without the recovery image** (installed with 1.0.0-beta.9.3.24 or earlier, when RECOVERY
stayed stock): write the release's `RECOVERY.img` once with mtkclient, and its `BOOTIMG.img` and `rootfs.ext4` as
under [Write the images](#write-the-images), since the older system cannot update itself. Back up RECOVERY first
(`python mtk.py r RECOVERY recovery-stock.img`) if your full backup predates this page.

**By hand** works as before: write a newer release's BOOTIMG and ANDROID (and RECOVERY, if its recovery image
changed) with mtkclient. The next update over Wi-Fi then backs up the system you wrote.

**Never write USRDATA again**: it would wipe your settings, library, music and the radio firmware.

## Known issues to plan around

- **No suspend.** The player can switch off but not sleep; the screen does not time out (the Y1 has no Power key to
  wake it).
- **Wi-Fi throughput is low** (1–8 Mbit/s measured, the Y2 about 45), uncalibrated defaults; still being measured.
- **Music over USB only** into the 2 GiB data partition; the built-in microSD is not used yet.
- **Updates: two paths are tested on the computer only.** An install cut off by a flat battery and a frozen
  installer (the recovery restarts the player after two minutes without progress) could not be caused on a Y1, which
  has no reset button; both resume or restore on the next start in the host tests and on the Y2. Keep the charger
  connected during an update.
- More, with evidence: the Y1 plans under [`docs/planning/`](planning/).

## Going back to stock

Write your own backups back with mtkclient: `boot-stock.img` to BOOTIMG, `recovery-stock.img` to RECOVERY,
`android-stock.img` to ANDROID and `usrdata-stock.img` to USRDATA (or BOOTIMG, RECOVERY and ANDROID from the stock
3.0.2 `rom.zip`, `boot.img`, `recovery.img` and `system.img`, plus your USRDATA backup). MISC holds no request once
an update is finished, and stock ignores it. Nothing else was written, so nothing else needs restoring. Copy your
music to a computer first.

**Only the recovery image back to stock**: write `recovery-stock.img` (or the `rom.zip`'s `recovery.img`) to
RECOVERY. Y2Linux keeps working, but **Restore Previous System** and the automatic way back after a failed update
need the Y1 recovery image; the next update over Wi-Fi writes it into RECOVERY again (every update carries one).

**A full restore has not yet been tested by the project.**
