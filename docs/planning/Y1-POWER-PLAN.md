# Y1 power (system milestone D)

Status: **planned** (2026-11-05), not admitted. The owner asked for milestone D after the audio PASS. Today the
Y1 cannot switch itself off: no power-off handler is built (`# CONFIG_POWER_RESET` unset), so
`busybox poweroff` halts with the PMIC still on and the pack draining; there is no battery percentage, no
low-battery shutdown, and the CPU runs at LK's fixed rate (`CPU_FREQ` and `CPU_IDLE` off). Each stage below
has its own admission.

## 1. Evidence

Offline analysis of the stock 3.0.2 kernel (agent notes `out/y1-power-notes.md`; "CONFIRMED" means seen in the
image's code or data, not on the device) and of the Y2's tree.

- **The Y1 has no power button.** The centre button is the MT6323 power key (`docs/knowledge/y1-hardware.md`),
  so it is also the only way to switch on from off, apart from plugging in USB.
- **Stock power-off** (`mt_power_off` c03a8c34, CONFIRMED): `rtc_bbpu_power_down` unlocks the RTC write
  interface (`0x8036` = `0x586a`, then `0x9136`, each followed by a write trigger), sets bit 5 of `RTC_CON`
  `0x803e` if `PDN1` `0x802c` & `0x1f00` is zero, then writes **BBPU `0x8000` = `0x4309`** (key `0x43`,
  `PWREN` and `AUTO` kept, bit 2 cleared so the power line drops) and triggers (`WRTGR` `0x803c` = 1, poll
  `0x8000` bit 6). It then loops; while a charger is present (`CHRDET`) it takes a watchdog reset in
  auto-restart mode, and LK presumably enters its power-off charging mode (LK path not traced, INFERRED).
- **Mainline power-off** (`drivers/power/reset/mt6323-poweroff.c`, the Y2's production path with the Y2's
  patch 0027, which checks the write and polls atomically): BBPU = `0x4300`, `WRTGR` = 1, poll. No unlock.
  The Y2 (same PMIC) powers off with it (full power-off retention in
  `docs/knowledge/hardware-final-power-evidence.md`). Whether the Y1's LK leaves the RTC write interface
  unlocked as the Y2's does is **unknown**; stage D0 reads it.
- **The Y2's charger-present power-off** (`y2_charger_power_off`, the RGU "charger" reset and LK reason 3, behind
  `innioasis,rgu`) is deliberately absent from the Y1 tree. Stock's Y1 path is the same kind of watchdog reset,
  but the Y1 LK's charging mode is unverified.
- **Stock battery gauge** (`oam_run`, CONFIRMED): software fuel gauge on OCV tables, temperature fixed at
  25 °C, three battery IDs chosen by AUXADC channel 0 (< 0.6 V id 0, < 0.8 V id 1, else id 2; which one the
  Y1 reads is UNRESOLVED, id 0 likely). Id 0: 4320 mV full … 3417 mV empty (full table in the notes),
  Q_MAX 1200 mAh. Power-off voltage 3400 mV; the kernel only drops the shown percentage to 0 and Android shuts
  down (INFERRED); the PMIC low-battery interrupt is unused.
- **The curve is for a 4.35 V cell**, as the charging plan found (stock CV 4.325 V). The Y1 charges to
  4.175 V under Linux, which on the id 0 curve is about 87 %.
- **The Y2's power daemon** (`y2-platform power-daemon`, `S04y2-power`): reads `BAT0` once a second,
  estimates SOC from `/etc/y2linux/battery-profile.json` (schema 1, ascending `[uV, %]` rows; invalid profile
  = no estimate), applies `/etc/y2linux/power-policy.json` (critical/low/recover 3.4/3.5/3.6 V, 5 samples,
  10 s grace), and owns shutdown: `/sbin/poweroff` and `/sbin/reboot` are its socket clients. Its shutdown
  steps write `BAT0/watchdog_powers_off` and open `/dev/watchdog`; the first does not exist on the Y1 (no
  `innioasis,rgu`). It needs AF_UNIX and `/run/y2`; `/data` is optional. PID1 already copies
  `etc/y2linux` overrides from `initramfs/y1-system`.
- **Stock CPU DVFS** (`mt_cpufreq_target`, CONFIRMED): OPPs chosen by efuse devinfo[18] bits 9:8; on every
  level **598, 806 and 1001 MHz run at 1150 mV**; 1209 and 1300 MHz need 1250 mV. A rate change moves the CPU
  mux (`0x10001000` bits 3:2) to a temporary source, reprograms `ARMPLL_CON1` `0x10205104`, waits 40 µs and
  moves back. VPROC (MT6323 `0x220`) is changed only through SPM DVFS slots. Idle: WFI, multi-core idle and
  SPM deep idle.
- **Mainline 6.18**: `mediatek-cpufreq` has no MT6572 entry; `cpufreq-dt` would bind the fork's dtsi
  automatically, ignore `opp-supported-hw` (offering 1209–1404 MHz) and reprogram ARMPLL with the CPU still
  on it. The fork's `mediatek-cpufreq` has an `mt6572` entry that parks the CPU on `MPLL_D2` during the
  relock. The Y1 PMIC wrapper's write policy does not cover VPROC or the RTC block.

## 2. Stages

| Stage | What | Image |
| --- | --- | --- |
| **D0** | Read-only baseline over SSH: RTC `0x8000`, `0x8036`, `0x802c`, `0x803e`; VPROC `0x220`; efuse speed bin; `0x10001000`, `ARMPLL_CON1`; AUXADC channel 0 (battery ID) | none |
| **D1** | Power-off on battery, then observed with USB attached | fifteenth boot |
| **D2** | Battery percentage and low-battery shutdown: the Y2's power daemon with a Y1 profile and policy | sixteenth boot |
| **D3** | CPU frequency scaling 598/806/1001 MHz at LK's fixed VPROC | seventeenth boot |

D1 comes first because D2's low-battery shutdown is worthless without a real power-off. D3 is last and
riskiest: it reprograms the CPU's own PLL.

## 3. D0 — baseline

Reads only, with the means used for the audio stage A baseline; no build, no flash. It decides: whether
mainline's unlocked BBPU write can work (`0x8036` state), that VPROC is at 1150 mV (`0x220` = `0x48`) before D3,
the efuse level, and the battery ID for D2's table.

## 4. D1 — power-off

- Kernel: the fourteenth fragment plus `POWER_RESET`, `POWER_RESET_MT6323`; the Y2's patch 0027 copied into
  `kernel/patches-y1` (manifest hashes); a `power-controller { compatible = "mediatek,mt6323-pwrc"; }` node
  under the PMIC. No RTC driver, no RGU, no `innioasis,rgu`.
- PMIC writes at power-off only: BBPU `0x8000` = `0x4300`, `WRTGR` `0x803c` = 1 (the Y2's values).
- Test 1 (battery): over SSH, `nohup sh -c 'sleep 20; sync; busybox poweroff' &`, then unplug USB. Expect the
  screen and backlight off within 30 s. After 10 minutes, a long press of the centre button boots LK and Linux;
  the boot OCV and `BAT0` voltage are compared with the value before (a halted-but-on player drains visibly).
- Test 2 (USB attached): the same with USB left in. Recorded only: stays off, restarts, or LK charging mode.
  No code follows from it in D1; a charger-present path is its own later admission.
- If the player does not switch off (D0 shows the RTC interface locked, or the write does not take), the
  fallback is stock's unlock sequence, which is a new admission. Recovery is a USB plug or the watchdog.

## 5. D2 — battery percentage and low-battery shutdown

- `initramfs/y1-system/etc/y2linux/battery-profile.json` from the stock table for the D0 battery ID,
  `provisional: true`, `source` naming the stock address. **Owner choice:** keep stock's curve (a full Linux
  charge shows about 87 %) or rescale the window 3417–4175 mV to 0–100 % (recommended: the shown percentage then
  means the usable charge). `design_capacity_uah` from Q_MAX.
- `power-policy.json`: the Y2's schema-2 values (critical 3.4 V = stock's power-off voltage), provisional.
- `S04y2-power` added to `y1-services`. Before the build, the daemon's shutdown steps are audited for the Y1:
  a missing `watchdog_powers_off` or `S90reborn`/`S41y2-connectivity` must be skipped, not abort the shutdown;
  if the daemon fails closed there, a narrow tolerance is the code change (Y2 behaviour unchanged, test added).
- Test: `y2-platform` power status shows voltage and SOC; `/sbin/poweroff` through the daemon powers off as
  in D1; the low-battery path with a synthetic policy override (critical above the present voltage) in
  `/run`, not by draining the pack; charging keeps working.

## 6. D3 — CPU frequency scaling

- Precondition: D0 reads VPROC at 1150 mV. All three admitted rates are 1150 mV on every efuse level, so **no
  voltage change**: the `proc` supply is a fixed 1.15 V regulator with no hardware behind it.
- Driver: the fork's `mediatek-cpufreq` MT6572 entry ported as a `patches-y1` overlay (intermediate
  `MPLL_D2` during the relock); `cpufreq-dt` kept off. A Y1 OPP table with only 598000, 806000 and 1001000 kHz;
  default governor `schedutil`; no `CPU_IDLE` (WFI stays the idle).
- Test: `scaling_available_frequencies` lists the three rates; each set with `userspace` and checked through a
  timed loop and `ARMPLL_CON1`; a ten-minute load on both cores with the governor switching; charging,
  display, audio and SSH unaffected. A hang ends in the watchdog reset.

## 7. Risks

A power-off that only halts (D1 checks the drain). With USB attached, the PMIC may re-power the player or
leave it off without charging (D1 test 2 only observes). The wrong battery ID gives a wrong percentage
(D0 reads it; the profile is provisional). A PLL relock fault hangs the CPU (watchdog reset; the image is
undone by restoring the fourteenth boot).

## 8. Out of scope

Suspend and SPM deep idle, VPROC scaling and rates above 1001 MHz, the RGU/LK charging-mode path, an RTC
driver and clock, the Y2DATA partition (milestone B) and the player UI (milestone E).

## 9. Admission

| Stage | Item | Bound |
| --- | --- | --- |
| D0 | **Read the power registers** | reads only, listed in §2; no write, no image |
| D1 | **Power off the PMIC** | BBPU `0x4300` and `WRTGR` 1 at power-off only, mainline's driver with the Y2's patch 0027; no RTC unlock, no RGU, no other PMIC write |
| D2 | **Run the power daemon** | Y2 daemon unchanged except a tolerance for missing Y2-only files; Y1 profile and policy in the RAM overlay; shutdown through D1's path only |
| D3 | **Scale the CPU clock** | 598/806/1001 MHz only, CPU mux and ARMPLL through the fork's driver with the intermediate clock; no VPROC or other PMIC write |
