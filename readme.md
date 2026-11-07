# Y2Linux | Y1 

This is an attempt to get the same stack running on an Innioasis Y1 player which is based on slightly different hardware.

Working so far:
- Booting
- USB network with ssh
- Video, Audio
- Can read battery level, can charge battery
- Can run Y2Rebord
- Wifi if firmware is installed
- Bluetooth if firmware is installed

Not yet done
- No power management (can power of but not go into suspend / auto suspend)


This has all been coded by Clause with a lot of bits and pieces grabbed from MediaTek repositories or reverse engineered from the stock android image

Some projects for reference:

- mt6572-mainline Linux fork: https://github.com/mt6572-mainline/linux. The MT6572 clock, pinctrl, power-domain and display drivers, and the mt6572-fork.dtsi device tree, were backported from commit 8d427adfb611bcdf4326efc69f44a23c14871b20.
- Mainline Linux 6.18.56: https://kernel.org. This is the base kernel. MT6572 SMP, musb, the MediaTek T-PHY, mtk-pm-domains, Lima, the MT6323 drivers and others come from it.
- mtkclient 2.1.4.1: https://github.com/bkerler/mtkclient. Every flash, read-back and eMMC backup went through it.
- uMTP-Responder (umtprd) v1.9.1: https://github.com/viveris/uMTP-Responder. This is the MTP server, with the Y2 project's own patches on top (buildroot/package/y2-mtp/ in the repo).
- Buildroot: https://buildroot.org. It builds the Y2's root filesystem, which the Y1 now runs.
- Mesa Lima driver: https://docs.mesa3d.org/drivers/lima.html, for the Mali-400 GPU that Reborn renders through.
- Chris Hendrickson's APT32F click-wheel driver, from artificery-dev/linux, commit 53fb57bb99c9. That's what the wheel driver's header credits. I believe it's on GitHub at https://github.com/artificery-dev/linux, but the source only names the repo, not a URL.
