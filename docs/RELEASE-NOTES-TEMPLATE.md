# Release notes format

The text of a GitHub release (`release.py publish --notes`) has two kinds of readers. People on the GitHub release
page, and people inside an updater, such as the community
[Innioasis Updater](https://github.com/y1-community/Innioasis-Updater), which shows the same text but downloads and
flashes the player itself. Write it for someone who uses the player, not for a developer: what they get, where to find
it, what was fixed, what to watch out for. Technical detail belongs in the release report under `docs/validation/`.

Everything that only makes sense on the GitHub page goes between `<!-- github-only -->` and
`<!-- /github-only -->`. That covers the SP Flash Tool install steps, how it was tested and the file list. Each
marker sits on a line of its own with a blank line before and after. GitHub hides the comments, and an updater that
knows the markers leaves out what is between them. `release.py publish` refuses notes without balanced markers and
wraps the "Sources unchanged" section it adds itself.

## Rules

- First line: the version in bold and one short phrase of what it brings.
- **How to update** comes first, because most readers already run Y2Linux.
- **What's new** and **Fixed**: one bullet each, the name of the feature in bold, then where to find it on the
  player (the real menu path) and what it does for the listener. No internal names, file names or ledger numbers.
  An issue number only where it helps the person who asked.
- **Known issues**: short, with what to do about it. End with error reports and Restore Previous System.
- Link the plain-words release report.
- GitHub-only part: Install from stock, How it was tested, Files.
- Last line, outside the markers: the beta disclaimer and licences.

## Template

```markdown
**Y2Linux 1.0 Beta X.Y**: one short phrase.

## How to update
Already on Y2Linux: Settings → System → Software Update → **Update Now**. Your music, playlists, settings, Wi-Fi networks and Bluetooth pairings stay. (On Beta 9.2.8 and older: Settings → About & Updates → Check for Updates → **Update Now**.)

## What's new
- **Feature**: where to find it, and what it does for you.

## Fixed
- **What was broken**: what you noticed, and that it works now.

## Known issues
- Issue, and what to do about it.
- Something wrong? Send an error report (Settings → System → Error Reports). **Restore Previous System** (Settings → System → About, tap Version seven times, then Diagnostics) takes you back to the version you had, with your data kept.

More details in plain words: **[Beta X.Y report](https://github.com/SchulzCode/Y2Linux/blob/main/docs/validation/Y2-LINUX-1.0-BETA-X.Y.md)**.

<!-- github-only -->

## Install from stock (SP Flash Tool)
You need SP Flash Tool v5.2032, a USB cable, **`Y2Linux-1.0.0-beta.X.Y-first-install.zip`** below and the **Innioasis Y2 firmware package v3.2.0** from Innioasis (only this version works; it is also your way back to stock).

1. Unpack both.
2. SP Flash Tool → **Download Only** → load `MT6582_Android_scatter.txt` **from the Y2Linux zip**. Exactly BOOTIMG, RECOVERY, ANDROID and USRDATA are ticked.
3. Click Download and connect the switched-off Y2; wait for the green tick.
4. Unplug and press Power: the player sets up its Internal Storage (a few seconds), then Y2Linux and Reborn start, music works.
5. Connect the Y2 to the computer and **copy** (don't move) `system.img` from the Innioasis package into the **Firmware** folder on **Internal Storage**. The player takes the Wi-Fi/Bluetooth firmware from it and turns the radios on.

Full guide: [INSTALL](https://github.com/SchulzCode/Y2Linux/blob/main/docs/INSTALL.md) · How updates work: [Recovery and updates](https://github.com/SchulzCode/Y2Linux/blob/main/docs/RECOVERY-AND-UPDATES.md) · Storage: [Storage](https://github.com/SchulzCode/Y2Linux/blob/main/docs/STORAGE.md) · [Audiobooks](https://github.com/SchulzCode/Y2Linux/blob/main/docs/AUDIOBOOKS.md) · Headphone settings: [Headphone output](https://github.com/SchulzCode/Y2Linux/blob/main/docs/HEADPHONE-OUTPUT.md) · [Error reports](https://github.com/SchulzCode/Y2Linux/blob/main/docs/ERROR-REPORTS.md) · Skins: [skin guide](https://github.com/SchulzCode/Y2Reborn/blob/main/docs/skins.md)

## How it was tested
What was reproduced and checked on a player, the new tests, the platform's full check, Reborn's workspace tests and the tests of the release images.

## Files
- `Y2Linux-1.0.0-beta.X.Y-first-install.zip`: images, scatter files and checksums for SP Flash Tool. The data image has no SSH key (developers: `tools/production/first_owner.py`).
- `manifest.json`, `manifest.sig`, `rootfs.ext4.gz`, `bootimg.img.gz`, `recovery.img.gz`: the signed over-the-air update (Ed25519 key `platform-v1-release-20261015`, sequence N).
- `Y2Linux-1.0.0-beta.X.Y-sources-buildroot-legal-info.tar` (licences, manifests, this project's packages) and the third-party, toolchain and Linux sources linked below: corresponding sources. Our own code: Y2Linux and Y2Reborn at tag `baseline-N`. The speaker amplifier driver is Chris Hendrickson's GPL-2.0 AW87559 driver. The headphone profiles are AutoEq results (MIT, Jaakko Pasanen); their licence ships with them.
- `SHA256SUMS` for the attached files.

<!-- /github-only -->

Beta software: use at your own risk. Not affiliated with Innioasis, MediaTek, Awinic, Last.fm, AutoEq, Strawberry, MusicBee or Google. Noto Sans CJK is licensed under the SIL Open Font License 1.1.
```
