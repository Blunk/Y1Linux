**Y1Linux 1.0.0 Beta VERSION**: the Y1 updates itself over Wi-Fi.

<!-- Draft for the first Y1Linux release with over-the-air updates (docs/planning/Y1-RECOVERY-PLAN.md, R6). Replace
VERSION and SEQUENCE, follow docs/RELEASE-NOTES-TEMPLATE.md, and pass the text to the GitHub release; the procedure
is in the plan's §13. -->

## What's new
- **Software updates over Wi-Fi**: Settings → System → Software Update. The player looks for a new version once a day on Wi-Fi; **Update Now** downloads it, restarts, installs it and restarts into the new version by itself. Your music, settings, library, Wi-Fi networks, Bluetooth pairings and radio firmware stay.
- **A way back**: if a new version does not start properly within three minutes, the player brings the previous one back by itself. **Restore Previous System** (Settings → System → About, tap Version seven times, then Diagnostics) goes back once by hand.

## Known issues
- An update needs the charger or at least half a battery. Keep the charger connected while it installs: an install cut off by a flat battery was not tested on a Y1.
- The download is about 60 MB; Wi-Fi on the Y1 is slow, so it can take several minutes.
- Something wrong? **Restore Previous System** takes you back to the version you had, with your data kept.

<!-- github-only -->

## Already running Y1Linux 1.0.0 Beta 9.3.24 or older
Your player cannot update itself yet. Write this release once with mtkclient, as in [INSTALL](https://github.com/Blunk/Y1Linux/blob/y1linux-1.0.0-beta.VERSION/docs/INSTALL.md#updating), **including `RECOVERY.img`**:

```sh
sha256sum -c SHA256SUMS
gunzip -k rootfs.ext4.gz
python mtk.py r RECOVERY recovery-stock.img      # keep it: your way back to the stock recovery
python mtk.py w ANDROID rootfs.ext4
python mtk.py w BOOTIMG BOOTIMG.img
python mtk.py w RECOVERY RECOVERY.img
```

**Never write USRDATA again**: it wipes settings, library, music and the radio firmware. From then on, updates come over Wi-Fi.

## First install
Follow [INSTALL](https://github.com/Blunk/Y1Linux/blob/y1linux-1.0.0-beta.VERSION/docs/INSTALL.md): back up the whole eMMC, build the data image (USRDATA) yourself, and write `rootfs.ext4`, `BOOTIMG.img` and `RECOVERY.img` from this release with mtkclient. How updates work on the Y1: [Recovery and updates](https://github.com/Blunk/Y1Linux/blob/y1linux-1.0.0-beta.VERSION/docs/RECOVERY-AND-UPDATES.md#on-the-y1).

## How it was tested
On a Type A Y1 (stock 3.0.2 bootloader): the recovery started from every way the Y1 starts, the kernel's write guard refusing the system partitions from the normal system, and test releases installed, cancelled, rolled back after a failed health check and restored by hand. A flat battery during an install and a frozen installer were tested on the computer (the updater's test suite) and on the Y2, not on a Y1.

## Files
- `BOOTIMG.img`, `RECOVERY.img`: the Y1 kernel with its init, and the recovery image that installs updates (mtkclient).
- `rootfs.ext4.gz`: the system (Y2ROOT) for the ANDROID partition; `gunzip` it before writing with mtkclient. The same file is the system part of the update.
- `manifest.json`, `manifest.sig`, `bootimg.img.gz`, `recovery.img.gz`: the signed over-the-air update (Ed25519 key `y1linux-release-20261112`, sequence SEQUENCE).
- `SHA256SUMS`: checksums of every file.

Y1Linux is a beta for one player model; use it at your own risk and keep your eMMC backup.

<!-- /github-only -->
