# Y1 charging: observation boot, then a first charging boot

Status: stage A done; stage B approved and **charging works** on the owner's player (2026-11-05,
[run 3](#13-stage-b-run-3--linux-charges-the-y1--2026-11-05): constant current at the 450 mA step, no
fault). Open: the constant-voltage phase, termination at 4.175 V and the timers (a full supervised charge),
deferred by the owner. Follows the
[third boot](Y1-THIRD-BOOT-PLAN.md) (PASS 2026-11-05: USB shell, read-only telemetry, charger input
measured).

## 1. Goal

Linux charges the Y1's battery from a PC USB port, within limits no higher than stock and with every
protection the Y2's charger has, so test sessions no longer end in "restore stock to charge".

Not in scope: wall chargers and charger-type detection (BC1.1), currents above the PC-port step,
charging while powered off (LK's charging mode), a fuel gauge or percentage, suspend, the display driver.

## 2. Evidence

### Y1 stock 3.0.2 (offline disassembly, [Y1 hardware](../knowledge/y1-hardware.md#offline-disassembly-usb-and-charging--2026-11-05))

| Item | Y1 stock | Y2 stock / Y2 Linux |
| --- | --- | --- |
| Charger | MT6323 internal linear charger, no external IC | same |
| PC port (SDP) request | 500 mA (PMIC floors to the 450 mA step) | same; Linux 450 mA |
| Wall (DCP/non-standard) | 1000 mA | 1100 mA; Linux default ceiling 650 mA |
| Pre-charge ends | 3400 mV (`BAT_PreChargeModeAction`, `0xd48`) | 3400 mV |
| CC → top-off | **4200 mV** (custom data `0xc07f0034`) | 4050 mV |
| Hardware CV | **4.325 V** (`pchr_turn_on_charging`) | Linux 4.175 V |
| Full / hold comparisons | 4279 mV (`0x10b7`), 3799 mV (`0xed7`) | Linux full ≥ 4.07 V rest, recharge < 4.11 V |
| Top-off timeout, termination samples | 10,800 s; 6 | same |
| Battery temperature | **constant 25 °C** (`force_get_tbat`) | same; Linux uses the PMIC die only |

The higher top-off and CV values suggest a 4.35 V cell; there is no label or datasheet. Linux's 4.175 V
termination is below every stock limit and undercharges such a cell by roughly 10–15 % of capacity, which
is accepted for a first charging boot.

### Y1 on Linux (third boot)

- Charger input (VCDT): 113 mV unplugged, 5222 mV on a PC port.
- ISENSE equal to the battery voltage within a few mV with no charging: the sense path reads.
- **BATON1 553 mV in both runs.** With stock's divider constants (16.9 kΩ pull-up to 1.8 V, 27 kΩ in
  parallel) that is an NTC of about 10.4 kΩ, about 24 °C for a common 10 kΩ thermistor. Without a
  thermistor the pin would sit near 1.1 V. ~~INFERRED: the Y1 pack has a working NTC that stock ignores.~~
  **Withdrawn by stage A (§9):** the reading never changes, warmed or not.

### The Y2 charger (`kernel/platform/mt6323-charger.c`, physically qualified on the Y2)

All PMIC access is through the MT6323 regmap; every write is read back. Hardware CV 4.175 V, software stop
at the first sample ≥ 4.175 V, latched fault ≥ 4.2 V; 24 h / 3 h / 3 h timers; die-temperature ladder
60/70/80/90 °C → 450/200/70/0 mA; 7 V input OVP, 4.3 V battery OVP, PMIC thermal shutdown; a PMIC hardware
watchdog (4 s) kicked every second, so a hung or panicked kernel stops charging within 4 s; charging
stopped at probe, unbind and shutdown; faults latch until a verified unplug.

What does not carry over unchanged:

1. **Protection checks expect exact register values** (`0x00e == 0x0005`, `0x018 == 0`, `0x020 == 0x0005`,
   `0x03c == 0x0020`, `0x044 & 0x703 == 0`, `0x000`, `0x002`, `0x00c`, `0x004` bit 2) that were
   observed on the Y2. On the Y1 they are unknown; a mismatch fails closed (no charging), which is safe but
   must be measured first (stage A).
2. **The USB contract** (`source.h`): the Y2's own USB driver supplies the input power supply, the USB
   current allowance, and BC1.1 coordination (it disconnects the gadget and routes D+/D- through PHY
   `USBPHYACR6` bit 23). The Y1 uses mainline USB, which offers none of this; a Y1 shim is needed (§4).
3. Y2 constants: 200 mΩ sense resistor and the taper rule's 10.2 mV, the +8 mV OCV offset, the 4.05/4.07/
   4.11 V lifecycle values. The termination and fault voltages are kept; the sense-based finish rule is
   used only as a voltage-plateau rule until the Y1's sense resistor is measured.
4. The Y2 PMIC write allow-list lives in the Y2's own PMIC-wrapper driver; the Y1 uses the mainline one,
   which has none. The Y1 build adds the same allow-list in front of the charger's writes (§4).

## 3. Stage A: observation boot (read-only)

The third-boot image plus:

1. **PMIC register reads.** `DEBUG_FS` and `REGMAP` debugfs (built without `REGMAP_ALLOW_WRITE_DEBUGFS`, so
   the file has no write path). A new shell command `pmic` reads a fixed list only, each register by
   offset in the debugfs file, never a sweep: `0x000`–`0x046` (charger block, including CHRDET in `0x000`
   bit 5), `0x0100` (chip ID), `0x724` (boot OCV), `0x758`/`0x76e` (ADC control, as set by the ADC driver).
   Read with and without the cable.
2. **NTC check.** `bat` gains BATON as an estimated temperature (stock divider, 10 kΩ B3435 assumed, shown
   as INFERRED). The owner warms the player in a hand for a few minutes: a real NTC reads warmer; a fixed
   resistor does not move.
3. **Load measurement.** ISENSE − BATSNS with the cable in (no charging) for the zero-current offset.

## 4. Stage B: first charging boot (planned, admitted after stage A)

- The Y2 charger source shared unchanged in behaviour for the Y2, as the ADC and wheel drivers are: Y1
  differences behind device-tree properties (expected protection values from stage A, the Y1 SDP-only
  source mode, NTC limits).
- A Y1 USB-source shim (`y1-usb-source.c`) implementing the `source.h` contract on mainline USB: an input
  power supply online from CHRDET; allowance ≥ 0 only after the gadget is **configured** by a PC, else
  −1 (the Y2 policy then charges at most 70 mA, below the awake load); BC1.1 not run (`-EAGAIN`), so a
  cable is a PC port only once enumerated and anything else stays at 70 mA.
- Limits: **450 mA** maximum (SDP step; owner ceiling fixed at 450 mA in the tree), hardware CV and
  termination 4.175 V, all Y2 timers and the die ladder.
- **Added for the Y1, because its pack appears to have an NTC:** charging pauses outside 5–40 °C pack
  temperature from BATON (only if stage A shows the NTC responds; otherwise not used and the plan says so).
- The same write allow-list as the Y2's PMIC wrapper, applied in the Y1 build.
- No power-off-to-charger path (`innioasis,rgu` absent): LK's charging mode on the Y1 is unverified.
- Test: from about 40–60 %, PC port, the owner watches; the shell reports state, selector, battery
  voltage, sense estimate, die and pack temperature every few seconds; the owner unplugs at any time.

## 5. Admission for stage A (now)

| Item | Why | Bound |
| --- | --- | --- |
| **PMIC register reads through regmap debugfs** | §2 point 1, the NTC, the sense offset | read-only by construction (no write path built); fixed register list; no sweep; nothing else changes from the third boot |

## 6. Admission for stage B (after stage A, not now)

Listed so the decision is visible early; each is decided with stage A's evidence in hand.

| Item | Bound |
| --- | --- |
| Enable the MT6323 charger (PMIC charger registers written) | Y2 driver's sequence and checks; allow-listed registers only |
| Charging current | at most 450 mA, only after USB enumeration by a PC; 70 mA otherwise |
| Charge voltage | 4.175 V hardware CV and termination, below stock's 4.2 V top-off and 4.325 V CV |
| PMIC charger watchdog | 4 s, kicked every second; a hang stops charging |
| ~~Pack-temperature window~~ | dropped: BATON does not respond (§9) |

## 7. Steps

1. Stage A: debugfs config, `pmic` command, BATON estimate; build; owner run with and without the cable,
   hand-warm test; record.
2. Stage B plan update with the measured values; owner decision on §6.
3. Stage B implementation: shim, shared charger with Y1 properties, allow-list; offline tests of the
   decision logic (as the Y2's charger tests); build.
4. Stage B run, supervised; record; audit entry.

## 8. Stage A build — 2026-11-05

The fourth Y1 variant (`y1-boot.sh fourth`): `kernel/config/y1-fourth-boot.config` (the third fragment plus
`DEBUG_FS`; `regmap-debugfs.o` built), `kernel/dts/innioasis-y1-fourth-boot.dts` (the third tree, new
model string), `initramfs/y1-fourth-boot.h`: debugfs mounted, the PMIC wrapper's regmap file found by
name, a shell command `pmic` that reads `0x000`–`0x046`, `0x100`, `0x724`, `0x758`, `0x76e` one line each
(the line's own address checked), printed as "now/at boot": PID1 snapshots the list before the session,
normally before the cable is in, because unplugging the cable also ends the shell; `bat` gains `pack N.N C?` from BATON (stock divider, 10 kΩ B3435
assumed; the `?` marks the assumption). The PID1 self-test checks the 553 mV → about 24 °C estimate.

Build `out/y1-fourth-boot-2`, BOOTIMG sha256
`dc9fb8488bde46d5b8fa2e341c863fa1f7756d82fb985c77637f7e1bce4318ff` (`0x18f800` bytes). The third-boot
image rebuilt byte-identical after these changes (`out/y1-third-boot-4` = `-3`). Physical status NOT_RUN.

## 9. Stage A result — 2026-11-05

Run on build `dc9fb848…`; the owner opened `/dev/ttyACM0` for the session. Raw output in
`evidence-private/2026-11-05-y1-charging-a/` (`pmic-1.txt`, `bat-1.txt`, `bat-warm.txt`, `dmesg.txt`).

**PMIC registers** ("now with the cable / at boot without it"): `000=0063/0001` (bit 5 CHRDET follows the
cable; bit 0 VCDT_HV_EN set), `002=00f2`, `004=0084/0004`, `006=001f`, `008=000f` (LK leaves the 70 mA
step), `00c=0001`, `00e=0005`, `014=0060/0020`, `018=0000`, `01a=0010`, `01e=0001`, `020=0005`,
`028=0022`, `02a=0024`, `02c=0001`, `02e=0090`, `03c=0000`, `040=4004`, `044=0000`, chip ID `2023`,
boot OCV `724=c4ca` (about 3.87 V on the BATSNS scale, INFERRED), ADC control `758=0010`,
`76e=00f8/0080` (request bits the ADC driver leaves set, as on the Y2).

Against the Y2 charger's fixed expectations (§2 point 1): `0x000 & 0x85 == 1` and `& 0xbd == 0x21`,
`0x004` bit 2, `0x00e == 0x0005`, `0x018 == 0`, `0x020 == 0x0005`, `0x044 & 0x703 == 0` **all hold on the
Y1**; `0x03c` is `0x0000` and the charger's prepare step sets bit 5 itself (expected `0x0020` afterwards).
`0x002`, `0x00c`, `0x006`, `0x008`, `0x028`–`0x02e` are written by prepare. **No Y1-specific protection
values are needed.**

**Sense offset:** ISENSE − BATSNS = −2…+2 mV with the cable in and no charging.

**BATON:** raw `10073` (553 mV) on every read, in both boots and after a 3–5 minute hand-warm test, while
BATSNS varies normally. A thermistor would move by at least a few counts. Either a fixed resistor (a
battery-ID resistor; consistent with stock never reading pack temperature) or a register (`0x71a`) that
the request does not refresh (stock reads its channel 5 from `0x722`). **Not a usable pack temperature:**
the §4 pack-temperature window is dropped from stage B; the die ladder alone protects, as on the Y2.
§2's NTC inference is withdrawn.

Stage A: done. Stage B awaits the owner's decision on §6 (without the pack-temperature item).

## 10. Stage B build — 2026-11-05

Owner approval of §6 (without the pack-temperature item): 2026-11-05.

- **Shared charger** (`kernel/platform/mt6323-charger.c`): `innioasis,usb-budget-source` skips BC1.1 and
  takes every source as a PC port (`Y2_SOURCE_SDP`); the Y2 policy then programs 450 mA when the USB
  allowance is ≥ 0 and 70 mA when it is −1. Without the property the Y2 path is unchanged
  (`test_charger`, `test_charge_limits`, `test_poweroff_charger` pass in the build sandbox).
- **Y1 USB budget source** (`kernel/y1/usb-source.c`): implements `source.h` for mainline MUSB. It provides
  the generic transceiver's `set_power` (empty in `phy-generic.c`), which MUSB calls with the composite
  layer's budget: 0 disconnected, 100 mA after a bus reset, 2 mA suspended, 500 mA configured
  (`CONFIG_USB_GADGET_VBUS_DRAW=500`). Allowance ≥ 0 only at ≥ 500 mA. BC1.1 entry points refuse.
  Registers the `y1-usb-input` power supply the charger's `power-supplies` names.
- **Write policy:** overlay `0005` (PMIC wrapper) gains `innioasis,pmic-charger-policy`: writes to
  `0x000`–`0x046` must pass the Y2's `y2_pmic_write_mask` and `y2_pmic_value_allowed` (`policy.h`, e.g.
  CV only 4.175 V, current only permitted table steps, OVP selectors only the stock ones) and may not
  change bits outside the mask. Other registers behave as before (the Y2 list does not admit the Y1's
  VGP1, VUSB or the ADC's channel-4 bit). Overlay `0014` (renamed `…-auxadc-charger-cells`) adds the
  charger's MFD cell with the CHRDET interrupt, as the Y2's `0024`; `0016`/`0017` add
  `CHARGER_INNIOASIS_MT6323`. Both `0005` and `0014` apply to every Y1 build; without the properties and
  the charger node they are inert, so the earlier Y1 kernels change bytes, not behaviour.
- **Tree and config:** `innioasis-y1-fifth-boot.dts` (charger node: ADC channels 7/5/6/3, the budget
  source, ceiling 450 mA, no `innioasis,rgu`), `y1-fifth-boot.config` (`POWER_SUPPLY`,
  `CHARGER_INNIOASIS_MT6323`, `USB_GADGET_VBUS_DRAW=500`); PID1's telemetry adds the charger's phase,
  limit, fault, current estimate and budget (`initramfs/y1-fifth-boot.h`).
- **Fixed on the way** (regressions from the second and third boots, both committed): `run.py`'s
  `prepare_overlay` regains its default patch directory (eight Y2 test files called the old signature);
  `test_power`'s ADC test now includes channel 4 (`0x718`), which the third boot made a valid sample.

Build `out/y1-fifth-boot-1`, BOOTIMG sha256
`595d8c9e9f8311200c62cfcbe61b577d28c049ef7739c9716741859607e484b6` (`0x195000` bytes), no compiler
warnings. Physical status NOT_RUN.

### Supervised run

From about 3.8 V, PC port. Claude reads `bat` and `charging_state` over the shell every 15–30 s.
Expected: `limit_ua 70000` before enumeration, `450000` once configured, `phase` CONSTANT_CURRENT, a
sense estimate near 450 mA, battery voltage rising. **Stop** (owner unplugs) on any of: `fault` ≠ 0,
die ≥ 55 °C, battery not rising after 5 minutes at 450 mA, battery ≥ 4.2 V, the player warm to the touch.

## 11. Stage B run 1 and correction — 2026-11-05

Run 1 (`595d8c9e…`; `evidence-private/2026-11-05-y1-charging-b/`): every driver probed
(`y1-usb-input`, `y2-mt6323-charger … USB-budget (no BC1.1) … ceiling 450000 uA, 4.175V`; the expected
`power off on a charger stays off: -19` without `innioasis,rgu`). With the cable in and `usb configured`
the charger stayed **inhibited**: `phase=SOURCE_DETECT source=1 source_valid=1 charge_limit_ua=0
active=0 fault=0x0 input_budget_ua=-1`; battery 3.776 V, die 40 °C. Nothing was charged.

Cause: `g_serial`'s configuration is self-powered (`legacy/serial.c:137`), so on configuration the
composite layer reports MaxPower 0, not `CONFIG_USB_GADGET_VBUS_DRAW`; a disconnect also reports 0, so
the budget alone cannot tell "configured" from "gone". Correction: the shim also reads the transceiver's
OTG state, which MUSB sets to `B_PERIPHERAL` on a host bus reset and `B_IDLE` on disconnect
(`musb_set_state`); configured = `B_PERIPHERAL` with a budget of 0 or ≥ 500 mA (100 mA = reset but not
configured, 2 mA = suspended). `USB_GADGET_VBUS_DRAW` is dropped from the config again. The bound of §6
holds: 450 mA only once a PC configured the gadget.

Build `out/y1-fifth-boot-3` (= `-2`, reproducible), BOOTIMG sha256
`24c845173294f7d0af67dae8112bb45b3e43d90d93c9e44e6fccbd1a5affe14f`. Physical status NOT_RUN.

## 12. Stage B run 2 and correction — 2026-11-05

Run 2 (`24c84517…`): the budget arrives (`input_budget_ua=500000`, `usb configured`), but the charger still
held at `phase=SOURCE_DETECT charge_limit_ua=0 stock_request_ua=0 active=0 fault=0x0` (3.768 V, die
38 °C). Cause: the budget-source branch of `y2_charger_detect` returned before the line the BC1.1 path
uses to clear `source_changed`, which the CHRDET interrupt sets on plugging; every poll then saw a changed
source and `y2_charger_target` returned 0. Correction: the branch clears it the same way. New test
`test_charge_limits.Worker.test_y1_usb_budget_source_charges_only_from_a_configured_pc` runs the real
`y2_charger_run` in budget-source mode (wall charger: the floor only; configured PC: 450 mA, selector 12;
unconfigured again: the floor, never a stale 450 mA); it fails without the correction (the stale-flag
assertion) and passes with it, as do the other charger tests.

Build `out/y1-fifth-boot-4`, BOOTIMG sha256
`ecab3af9acc52b313790a6dce904fbd8b75d8fe880065ce93a1a82a7590ab5cf`. Physical status NOT_RUN.

## 13. Stage B run 3 — Linux charges the Y1 — 2026-11-05

Run 3 (`ecab3af9…`; `evidence-private/2026-11-05-y1-charging-b/run3-*`), PC port, supervised over the USB
shell. As soon as the gadget was configured: `phase=CONSTANT_CURRENT active=1 charge_limit_ua=450000
selector=12 stock_request_ua=500000 class_ceiling_ua=500000 owner_ceiling_ua=450000 fault=0x0`;
charger input 5.22 V unloaded, 5.07 V under charge.

| Time | Charging s | Battery | Sense estimate | PMIC die | Watchdog pets |
| --- | --- | --- | --- | --- | --- |
| 08:13:28 | 25 | 3.852 V | 464 mA | 43.0 °C | 17 |
| 08:15:30 | 148 | 3.867 V | 454 mA | 46.6 °C | 88 |
| 08:18:44 | 341 | 3.875 V | 452 mA | 48.3 °C | 200 |
| 08:21:45 | 523 | 3.883 V | 459 mA | 48.9 °C | 305 |
| 08:25:47 | 765 | 3.893 V | 460 mA | 50.1 °C | 445 |

Constant current 445–469 mA on the 200 mΩ basis (unvalidated on the Y1, as on the Y2), battery rising
about 3 mV/min, no fault, no derating (`thermal_level 0`), the PMIC watchdog serviced throughout. The die
rises ever more slowly (about +0.5 °C/min after ten minutes) and stays below the 60 °C first derating step.
BATON read 10070 once while charging (10073 otherwise). Not yet observed: the constant-voltage phase,
termination at 4.175 V and the timers, which take a full charge.
