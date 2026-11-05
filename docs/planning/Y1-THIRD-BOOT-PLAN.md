# Y1 third boot: USB serial shell and read-only battery telemetry

Status: **PASS** on the owner's player (2026-11-05, [run 2](#11-run-2--usb-works--2026-11-05)); approved
by the owner with all four admission items the same day. Follows the [second boot](Y1-SECOND-BOOT-PLAN.md) (PASS 2026-11-05: clocks, pins, PMIC wrapper,
read-only eMMC/microSD, wheel and centre button on Linux 6.18).

## 1. Goal

A third test kernel that replaces the camera with a cable and measures the battery, still writing nothing
to the player's storage and switching on no charging:

1. **USB device mode.** The MT6572's USB controller runs as a peripheral; the player appears on the PC as
   a USB serial port (`/dev/ttyACM0`, the standard `g_serial` ACM function), carrying a command shell
   (§4: built into PID1, not busybox).
   Kernel log, `/proc`, `/sys` and command output are read over the cable instead of photographed.
2. **Battery telemetry, read-only.** The MT6323 PMIC's analogue converter reports battery voltage, the
   current-sense voltage, the charger input voltage, the BATON pin and the PMIC die temperature, every
   10 s on screen and on request in the shell. Whether a charger is plugged in is read, nothing is acted on.
3. Everything the second boot proved stays: screen output, read-only storage, wheel and centre button.

Not in scope: charging, USB host mode or any 5 V output on the connector, USB DMA, mass storage or network
gadgets, display driver, audio, suspend, Wi-Fi/Bluetooth, any write to the eMMC or the SD card, Buildroot.

## 2. Evidence

### Stock 3.0.2 kernel (offline disassembly, 2026-11-05)

From `evidence-private/2026-11-04-y1-rom-3.0.2/rea` (function and address in each case; details in
[Y1 hardware evidence](../knowledge/y1-hardware.md#offline-disassembly-usb-and-charging--2026-11-05)):

| Fact | Source |
| --- | --- |
| MUSB at virtual `0xf1100000` = physical `0x11100000`; interrupt 52 = GIC SPI 20, level low | `musb_probe` c0792298, `musb_platform_init` c07937f8 |
| USB2 PHY at `0x11110800`; frequency meter for slew-rate calibration at `0x11110f00` | `usb_phy_recover` c03a7ab4, `hs_slew_rate_cal` c03a75ec |
| Clocks: peripheral gate USB (CG_CTRL1 bit 13) and UNIVPLL | `usb_enable_clock` c03a750c, `musb_platform_enable` |
| **VUSB 3.3 V** switched on at USB init and never switched off in normal operation | `musb_platform_init`: `hwPowerOn(1, 3300, "VUSB_LDO")` |
| Charger present: PMIC register `0x0000` bit 5 (RGS_CHRDET) | `upmu_get_rgs_chrdet` |
| AUXADC: BATSNS ch7 ×4, ISENSE ch6 ×4, charger input ch4 ×369/39, BATON1 ch5; 1.8 V, 15 bit | battery meter |
| Charging by the MT6323's own linear charger; **no external charger IC** (the "FAN5405" text is a log string) | no fan5405/bq24 symbols, no charger I2C device |

### Mainline 6.18.56 and the MT6572 fork

- `drivers/usb/musb/mediatek.c` and `drivers/phy/mediatek/phy-mtk-tphy.c` are **byte-identical** in 6.18.56
  and the fork (git blob `c6cbe718…`, `f6504e0e…`). The fork's `mt6572.dtsi` (already ours as
  `kernel/dts/mt6572-fork.dtsi`) describes the controller (`mediatek,mt2701-musb`, SPI 20 low, clocks
  `usb` + `univ_48m`) and the T-PHY (`mediatek,generic-tphy-v1`, port at `0x11110800`); three fork boards
  enable both. The fork's `CLK_TOP_USB` is gate bit 13 of the same register group as stock. INFERRED to
  work: the fork publishes no test result.
- **One mismatch.** Mainline's v1 T-PHY runs a slew-rate calibration with the frequency meter at
  `sif + 0x100` = `0x11110100`; on the MT6572 the meter is at `0x11110f00`. Calibration would write to
  registers whose meaning on this SoC is unknown. The board tree sets `mediatek,eye-src = <4>` on the port,
  which skips the calibration and writes 4, the value the stock kernel itself uses when its calibration
  times out. The stock efuse trim (`VRT_VREF_SEL` from devinfo word 7) is not applied; the default stays.
- The MT6323 AUXADC has no 6.18 driver. The Y2's `kernel/platform/mt6323-adc.c` touches only the PMIC
  (regmap, no SoC registers) and is shared unchanged in behaviour, as the wheel driver was. It gains
  channel 4 (charger input, register `0x718`: CONFIRMED in stock `upmu_get_rg_adc_out_vcdt`; the stock
  register map also confirms the Y2 table's BATSNS `0x714`, ISENSE `0x716`, BATON1 `0x71a`, THR_SENSE1
  `0x71c`), only when the device tree sets `innioasis,charger-input`, so the Y2 exposes the same channels.

## 3. Device tree and kernel

`kernel/dts/innioasis-y1-third-boot.dts`: the second-boot tree plus

| Node | Setting |
| --- | --- |
| `&usb` | `okay`, `dr_mode = "peripheral"` (no host mode, no role switching, no VBUS output) |
| `&u2phy0`, port `u2port0` | `okay`; `mediatek,eye-src = <4>` (§2) |
| `pmic` regulators | adds `ldo_vusb` 3.3 V, `regulator-always-on` (§6) |
| `pmic` adc | `mediatek,mt6323-auxadc` child (Y1 overlay of `mt6397-core.c` adds only this MFD cell) |

Kernel config: the second-boot fragment plus `USB_SUPPORT`, `USB_GADGET`, `USB_MUSB_HDRC`,
`USB_MUSB_GADGET`, `USB_MUSB_MEDIATEK`, `NOP_USB_XCEIV`, `MUSB_PIO_ONLY`, `PHY_MTK_TPHY`, `USB_G_SERIAL`,
`IIO` and the ADC driver. `USB` (host stack), `USB_INVENTRA_DMA` and `U_SERIAL_CONSOLE` stay off; the kernel
log is read with `dmesg`, and boot never waits for a PC. Still no filesystem driver.

## 4. Test PID1 and the initramfs

~~The initramfs gains busybox from the locked inputs (as the Y2 rescue image).~~ **Changed at
implementation:** the Y2 rescue image takes busybox from its Buildroot target, which the Y1 does not have;
an armv7 busybox would be a new build input. The shell is built into PID1 instead
(`initramfs/y1-third-boot.h`): `help`, `bat`, `dmesg`, `cat PATH`, `ls PATH`, `disks`, `reboot`. It has no
command that writes, which is stricter than admission item 4. PID1 (`y1-init.c`, a third mode; the first
two stay byte-identical):

1. Watchdog, banner, storage check as in the second boot (markers and digests), then `BLKROSET` on every
   `mmcblk*` device, which stays set for the whole session.
2. Battery line every 10 s on screen: BATSNS mV, ISENSE mV, charger input mV, BATON mV, die °C, charger
   present yes/no. Below 3500 mV it says so in large text (the test kernel cannot charge).
3. Opens `/dev/ttyGS0` (non-blocking; reopened after the PC hangs up) and answers one command per line.
   PID1 keeps feeding the watchdog; key events and USB state changes appear on screen; the session ends
   with `reboot` in the shell.

Host side: `cdc_acm` is present on the owner's PC. Opening `/dev/ttyACM0` is the owner's call per session;
with that permission Claude reads logs and runs **read-only** commands over it.

## 5. What the owner sees

USB plugged in: `Y1 THIRD BOOT: USB configured` on screen, `ttyACM0` on the PC, a `# ` prompt in
`picocom /dev/ttyACM0`. Unplugged: the battery lines keep updating; charger present = no.

## 6. Safety and admission

| Item | Why | Bound |
| --- | --- | --- |
| **USB device mode**: MUSB controller and T-PHY | goal 1 | peripheral only, PIO only, ACM function only; slew-rate calibration skipped (eye-src 4, the stock fallback); host mode and VBUS output not built |
| **Enable MT6323 VUSB at 3.3 V** | the PHY's analogue supply | the stock voltage; stock never switches it off; no other new regulator |
| **PMIC AUXADC sampling** | goal 2 | writes only the ADC request and buffer-enable bits (`0x758` bit 4, `0x76e` channel bits) and restores them; reads the calibration fuses; **no charger register is written** |
| **A shell over USB** | goal 1 | storage stays `BLKROSET` and no filesystem driver exists; any write command, `blockdev --setrw` included, is out of scope and needs its own decision |

Failure modes: USB does not enumerate (the screen shows the controller state; nothing else is affected,
and the cable can be removed); the PHY misbehaves electrically (only PHY bits and the stock VUSB voltage
are touched; unplug); a driver hangs (watchdog resets in 15 s). The test kernel never charges: battery
under 3.6 V before a run means charging under stock first (`boot-stock-from-player.img` to BOOTIMG).
Recovery unchanged: BOOTIMG through the preloader.

## 7. Charging, the next boundary

Not admitted here; recorded so the fourth plan starts from evidence. Stock charges with the MT6323 linear
charger: constant voltage **4.325 V**, 500 mA from a PC port (SDP/CDP), 1000 mA from a wall charger,
pre-charge below 3.4 V, a BC1.1 charger-type detection, and a hardware charger watchdog. Stock's battery
temperature is a **constant 25 °C**: the NTC is not read. The pack capacity and sense resistor are in runtime
data not present in the image. The Y2's `mt6323-charger.c` drives the same charger. Telemetry from this
boot (voltage under load, BATON with and without the cable) decides how Linux may charge.

## 8. Steps

1. Shared ADC driver: channel 4; Y1 overlay of `mt6397-core.c` (auxadc cell only); manifest entries.
2. Third-boot tree and config; build to check the drivers bind offline (config check, DTB check).
3. PID1 third mode, busybox in the initramfs, telemetry; QEMU self-test; reproducible build.
4. Owner run: write, read back, boot with and without the cable, photograph, open the serial port.
5. Record the result; audit entry; charging plan.

## 9. Build result — 2026-11-05

Implementation:

- `kernel/platform/mt6323-adc.c` (shared with the Y2): channel 4 behind `innioasis,charger-input`; the
  thermal zone is registered only with `THERMAL_OF` (the Y1 test kernel has no thermal framework, and the
  6.18 stub would fail the probe). The Y2 sets neither, so its behaviour is unchanged; its rebuild check
  is owed together with the earlier one.
- Y1 overlays `0012`/`0013` (IIO Kconfig/Makefile: `INNIOASIS_MT6323_AUXADC`), `0014` (`mt6397-core.c`:
  the `mt6323-auxadc` MFD cell only); added files `drivers/iio/adc/innioasis-mt6323-adc.c`,
  `power-math.h`, `charge-table.h` from `kernel/platform`.
- `kernel/dts/innioasis-y1-third-boot.dts`, `kernel/config/y1-third-boot.config`,
  `initramfs/y1-third-boot.list`, `initramfs/y1-third-boot.h`; `y1-boot.sh third`; `y1_boot.py` checks the
  third tree (peripheral mode, eye-src 4, VUSB 3.3 V, the ADC node) and exactly the admitted regulators
  per variant.

Build `out/y1-third-boot-1`: config check passed (`USB` host stack off, MUSB gadget PIO-only, T-PHY,
`g_serial`, the ADC), PID1 self-test passed under QEMU (SHA-256 and the telemetry arithmetic).
The `0014` MFD cell applies to every Y1 build: the second-boot rebuild (`out/y1-second-boot-11`) has the
same PID1, device tree and config as build 10 but a different kernel (the inert cell; its driver is not
built there). The third-boot build is reproducible (`-1` = `-2`).

## 10. Run 1 and correction — 2026-11-05

Run 1 (`3c83f1c9…`, photos `evidence-private/2026-11-05-y1-third-boot/run1-*.jpg`):

- Storage: all six digests `MATCHES backup` again; `disks: 5 set read-only`.
- **Telemetry works** (`iio:device0`): battery 3802 mV falling to 3774 mV over about two minutes; ISENSE
  equal to the battery voltage within a few mV (no charging current, as expected); BATON 553 mV; die
  33–34 °C. **Charger input (VCDT, channel 4) is CONFIRMED:** 113 mV without the cable, 5222 mV with it
  (one reading of 179 mV while plugging).
- **USB did not connect.** `/dev/ttyGS0` opened, but the controller (`musb-hdrc.1.auto`) stayed
  `not attached` with the cable in, and the PC logged no device at all (only the preloader from flashing).
- Cause, INFERRED: mainline's T-PHY sets `RG_VBUSVALID`/`RG_AVALID` without their force bits, so the
  controller relies on the PHY's own VBUS comparator; stock `usb_phy_recover` forces the whole session
  (`U2PHYDTM1` `|= 0x2e`, `RG_SESSEND` clear, force bits 9–13), consistent with a PHY that does not sense
  VBUS on this board (the PMIC detects the charger).

Correction: overlay `0015` adds `innioasis,force-session-valid` to the T-PHY (the stock bits, in
`power_on`, only with the property); the third tree sets it. PID1 now also shows the kernel's USB/PHY log
lines on screen before the session. Build `out/y1-third-boot-3`, BOOTIMG sha256
`d6a38c82ccb3f27301bd3f49c1df7990e20295b2441af8211207ce02eff41b6e`. Physical status NOT_RUN.

## 11. Run 2 — USB works — 2026-11-05

Run 2 (`d6a38c82…`, photos `run2-*.jpg`): kernel USB log on screen: `usb_phy_generic …: dummy supplies
not allowed for exclusive requests (id=vbus)` (the glue's generic transceiver; harmless in device mode),
`g_serial gadget.0: Gadget Serial v2.4`, `g_serial ready`. Without the cable `usb not attached`; with it
**`usb configured`**, charger 5222 mV. The PC (xhci) enumerated a **high-speed** device `0525:a4a7`
"Gadget Serial v2.4", "Linux 6.18.56-y1-third-boot with musb-hdrc", and `cdc_acm` created
`/dev/ttyACM0`. The force-session correction (overlay `0015`) is thereby CONFIRMED as the fix. Goal 1's
transport works; the shell itself is untested until the owner opens the port (on the owner's PC
`/dev/ttyACM0` is group `dialout`, which the account is not in).

The owner opened the port for this session (2026-11-05, account given access to `/dev/ttyACM0` by ACL).
Over it: `help`, `bat` (battery 3690 mV, charger 5222 mV, `usb configured`), `disks` (every disk `ro=1`)
and `dmesg`, the player's **complete kernel log** for the first time (125 lines,
`evidence-private/2026-11-05-y1-third-boot/run2-dmesg.txt`). It shows no error from any driver this boot
uses. New facts from it:

- LK leaves **VGP1 at 2.8 V**; Linux raises it to the stock 3.0 V (`vgp1: Bringing 2800000uV into
  3000000-3000000uV`). The wheel works either way.
- The UART binds (`11005000.serial: ttyS0 … is a 16550A`): the second boot's goal 4 holds.
- MT6323 chip ID `0x2023`; the AUXADC's die-temperature calibration fuses are valid (`die calibration=1`).
- eMMC `H4G1d` 3.61 GiB, high speed; microSD SDXC 116 GiB, high speed.
- Expected warnings only: the `/cpu` node count (two cores, kernel built without SMP), `Failed to locate
  of_node` for the MT6323 RTC, LED and power-controller cells this tree does not describe, and the
  generic transceiver's `id=vbus` dummy-supply note.

**Third boot: PASS** (2026-11-05). Goal 1 (USB device mode, high speed, ACM shell), goal 2 (telemetry,
charger input confirmed) and goal 3 (storage read-only and matching; the wheel and keys are unchanged from
the second boot, not re-exercised) are met. Nothing was written to storage; no charger register was
written. Next boundary: charging (§7).
