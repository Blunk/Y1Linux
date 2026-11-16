# Y1 power (system milestone D)

> Part of the [single Y1 build](../knowledge/y1-hardware.md#single-build--2026-11-09). The step builds named
> below (the *N*th boot, its variant, tree, fragment and PID1 header) are the record of the test images that ran
> on the player; their files are folded into that build.

Status: **D0 done**, **D1 PASS** (2026-11-05, [run](#12-d1-run--2026-11-05)), **D2 running** ([run](#14-d2-run--2026-11-05): percentage, charging state and daemon-coordinated restart verified; the low-battery shutdown test deferred by the owner); **D3 on the owner's Y1, acceptance PASS** ([build](#15-baseline-tools-and-d3-build--2026-11-15)); **D4 PASS**, **D5 PASS** (hotplug and the screen-off policy) ([idle plan](#16-idle-states-d4d7--2026-11-15)). The owner asked for milestone D after the audio PASS. Today the
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
| **D4** | Tick: the Cortex-A7 architected timer, tickless idle, high-resolution timers; the GPT as broadcast | `out/y1-boot-d4` |
| **D5** | CPU1 hotplug with power-off through the SPM's `FC1_PWR_CON` sequence; CPU1 off while the screen is dark | `out/y1-boot-d5b` |
| **D6** | SPM deep idle on CPU0 alone: stock's dpidle firmware, CPU0 dormant, L2 retained, GPT wake, VPROC 1.05 V through the PWRAP DVFS slot; the Y2's coordinator (§17) | admitted, D6a–D6d |
| **D7** | Suspend: stock's suspend firmware, MT6323 sleep mode (VPROC 0.85 V when SRCLKEN drops) | planned |

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

VPROC scaling of a running CPU and rates above 1001 MHz (deep idle and suspend are planned in §16, each with its
own admission), the RGU/LK charging-mode path, an RTC driver and clock, the Y2DATA partition (milestone B) and the player UI (milestone E).

## 9. Admission

| Stage | Item | Bound |
| --- | --- | --- |
| D0 | **Read the power registers** | reads only, listed in §2; no write, no image |
| D1 | **Power off the PMIC** | BBPU `0x4300` and `WRTGR` 1 at power-off only, mainline's driver with the Y2's patch 0027; no RTC unlock, no RGU, no other PMIC write |
| D2 | **Run the power daemon** | Y2 daemon unchanged except a tolerance for missing Y2-only files; Y1 profile and policy in the RAM overlay; shutdown through D1's path only |
| D3 | **Scale the CPU clock** | 598/806/1001 MHz only, CPU mux and ARMPLL through the fork's driver with the intermediate clock; no VPROC or other PMIC write |
| D4 | **Tickless idle** | kernel configuration only (arch timer, `NO_HZ_IDLE`, `HIGH_RES_TIMERS`); no register write outside the timers' own drivers; BOOTIMG only |
| D5 | **Power CPU1 off** | stock's `FC1_PWR_CON`/`CPU_FC1_L1_PDN` sequence and `BOOT_SLV_CFG`/`ADDR` (§16); CPU1 only, every poll bounded |
| D6 | **Deep idle on CPU0** | §17, D6a–D6d as bounded there; admitted by the owner 2026-11-16 |

## 10. D0 result — 2026-11-05

Owner-approved. Fourteenth boot over SSH, reads only: PMIC registers by offset from the regmap debugfs file
(the full dump reads every PMIC register and took over a minute, so it was stopped), MMIO with `devmem`
(`evidence-private/2026-11-05-y1-power-d0/regs.txt`). Battery 3.846 V, not charging.

| Read | Value | Meaning |
| --- | --- | --- |
| RTC_PROT `0x8036` | `0x9136` | the second unlock key: LK left the RTC write interface unlocked (INFERRED from the stock sequence) |
| BBPU `0x8000` | `0x000d` | `PWREN`, power line on (bit 2), `AUTO` |
| PDN1 `0x802c` / RTC_CON `0x803e` | `0x0000` / `0x0426` | stock's `RTC_CON` bit 5 already set; nothing to add before the BBPU write |
| VPROC VOSEL `0x21e`–`0x224` | `0x48` each | **1150 mV** ((0x48 × 6.25) + 700): D3's precondition holds |
| Efuse `0x10009180` | `0x2800` | speed bin (bits 9:8) **0**: stock's widest table (up to 1300 MHz); the fork's offset (INFERRED as stock's devinfo[18]) |
| CPU mux `0x10001000` | `0x4` | bits 3:2 = 1: the CPU runs from ARMPLL |
| `ARMPLL_CON1` `0x10205104` | `0x0009a000` | PCW `0x9a000`, post-divider /1: **1001 MHz** (26 MHz × PCW / 2^14), with the VCO at 1001 MHz |
| Battery ID (AUXADC ch 0) | not read | the Y1 IIO device exposes channels 4–7 and the die temperature only; a conversion needs a write, outside D0 |

Consequences: **D1** can use mainline's unlocked BBPU write as on the Y2. **D2** uses battery id 0 (provisional; the
ids differ mainly in the last few percent: empty at 3417, 3400 or 3327 mV). **D3** needs no voltage change, but
LK runs ARMPLL at /1 with its VCO at the bottom of the fork's 1000–1800 MHz range; 598 and 806 MHz need the /2
post-divider (VCO 1196 and 1612 MHz), as stock's formula does, so the driver's PLL path must be checked for that.

## 11. D1 build — 2026-11-05

Owner-approved. `kernel/config/y1-fifteenth-boot.config`: the fourteenth fragment plus `POWER_RESET`,
`POWER_RESET_MT6323` (no `RTC_CLASS`); `kernel/patches-y1/0030-mt6323-poweroff-atomic.patch`, the Y2's 0027
byte for byte (manifest hashes as the Y2's); `innioasis-y1-fifteenth-boot.dts`: the fourteenth tree plus
`power-controller { compatible = "mediatek,mt6323-pwrc"; }` under the PMIC; PID1 and initramfs as the fourteenth
(`y1-init` `f00d5397…` unchanged). Build `out/y1-fifteenth-boot-1`, BOOTIMG sha256
`716f0c69210f7232c9cc70331ac366d16b74affe53d6ea7633ed8a953c17197e` (`0x323000` bytes).

## 12. D1 run — 2026-11-05

The image as flashed reports `6.18.56-y1-fourteenth-boot`: the fifteenth fragment kept the fourteenth's
`CONFIG_LOCALVERSION` (cosmetic; the device tree model reads "fifteenth-boot"). `mt6323-pwrc` bound; no new warning.
SSH host keys are new at every boot (RAM, as planned until milestone B), so each boot replaces the entry in
`out/y1-ssh/known_hosts`.

- **Test 2, USB attached** (the owner did not unplug in time; `evidence-private/2026-11-05-y1-power-d1-*`):
  `busybox poweroff` at 16:05:06; the player came straight back: Innioasis logo, Linux booting, SSH up about 15 s
  later. No charging screen. The boot OCV read the same 3.852 V as the boot before, so the PMIC most likely
  never lost power (INFERRED). With a charger present the mainline path is a restart, not an off state; the
  stock path (a watchdog reset into LK's charging mode) is the later, separately admitted option.
- **Test 1, battery only:** `busybox poweroff` at 16:11:02 with USB unplugged; the screen went dark and the
  player stayed off. The owner held the centre button at about 16:13:50 (about 3 minutes off, not the planned 10);
  it switched on through LK into Linux. The PMIC had really been off: the boot OCV register was sampled
  afresh (`0x724` `0xc979` before, `0xc476` after; `voltage_ocv` 3.852 V before, 3.858 V after, the pack
  rested), BBPU back at `0x000d`. Charging normal on that boot (450 mA source, no fault).
  **Power-off on battery: PASS.**
- **Unrelated finding, the charger:** on the first boot of this image the charger latched `fault=0x1` (I/O) at
  5.4 s, `charger inhibit not verified: -1` (the Y1 PMIC wrapper's policy refused a write of the stop
  sequence, `-EPERM`); the following stop succeeded (`stop_error=0`), but the fault stays latched until the
  cable is removed, so the player did not charge on that boot. The next boot charged normally (450 mA
  source, no fault). D0's "Not charging" on the fourteenth boot is likely the same. Open: which register was
  refused (the driver reports only the error code).

## 13. D2 build — 2026-11-05

Owner-approved ("make the power daemon D2"); the percentage choice is the plan's recommendation (rescaled,
like the Y2's own profile, which also ends at 4.175 V = 100 %).

- **Profile** (`initramfs/y1-power/etc/y2linux/battery-profile.json`, provisional): stock battery id 0, DOD → SOC,
  rescaled so 4.175 V (stock 87.3 %) is 100 %: 23 rows from 3.417 V = 0 % (checked against the daemon's
  `ascending_table` rules: 2–128 rows, strictly ascending voltage and SOC, 0 and 100 at the ends);
  `design_capacity_uah` 1048000 (Q_MAX 1200 mAh × 0.873); no resistance table (stock's `r_profile` not extracted);
  the Y2's filter, rest and offset values.
- **Policy** (`power-policy.json`, provisional): the Y2's values; the 3.4 V floor is also stock's Y1 power-off
  voltage.
- **Audit of the daemon's shutdown on the Y1** (`coordinator/steps.rs`): every step records its result and
  continues. `watchdog_powers_off` fails (no `innioasis,rgu`: "powers_off_unavailable") and `/dev/watchdog` is
  armed: a stalled shutdown ends in a **restart**, not off. `long_press_reset` is absent (step fails, harmless).
  `S90reborn stop` (the Y2's) finds no supervisor; the daemon then kills `reborn` by identity. The radio stops
  find no Y1 radio (rfkill and `18070000.connectivity` absent; `bluetoothctl` without D-Bus). The daemon assumes
  the Y2's 31 s watchdog; the Y1's is 15 s (the fork's `timeout-sec`): the shutdown's duration is measured
  before deciding whether that matters.
- **Scripts:** `S03y1-data` creates Reborn's RAM `/data` before `S04y2-power` (its journals live there);
  `S90y1-reborn` keeps a `data` action and stops restarting Reborn once `/run/y2/shutdown.json` exists. PID1
  installs the three new files under `Y1_TWENTIETH_BOOT`; the service list gains `S03y1-data S04y2-power`.
- Kernel unchanged (the nineteenth's config and tree, new names). Build `out/y1-twentieth-boot-1`, BOOTIMG sha256
  `335369e9a0ad18600319e128fc8fb2ddd31b98738b27f327672800a7a49c9449` (`0x33a800` bytes).

## 14. D2 run — 2026-11-05

`evidence-private/2026-11-05-y1-power-d2/`. Twentieth boot (build 1): the daemon runs under `platform-supervise`;
`/run/y2/power.json`: 94 %, charging, `voltage_estimate`, provisional, 976 mAh of 1048 mAh; the owner sees the
percentage and the charging mark in Reborn. Reborn's Settings have no Restart or Power Off in this version (they
are on the "service" page that the Y2 opens with its power key, absent on the Y1), so the shutdown tests used
`/sbin/reboot` (the daemon's client), USB attached:

- **Restart 1:** intent pending at once; no acknowledgement, so the 10 s grace ran out; the 13 steps then ran in
  about 0.2 s (watchdog armed; failed harmlessly: the two Y2 boot-journal steps, `long_press_reset` absent;
  `watchdog_powers_off` not attempted for a restart); the player came back into Reborn by itself. The owner saw
  Reborn's shutdown screen. **The shutdown is far inside the Y1's 15 s watchdog; no timeout change needed.**
- **Restart 2**, Reborn's log recorded too: Reborn presented its shutdown transition (75 frames) and its
  acknowledgement was refused, `ack_client_identity_changed`: the daemon accepts it only from the pid in
  `/run/reborn/process.pid`, which the Y2's `reborn-supervise` writes and `S90y1-reborn` did not. Fix: the Y1 loop
  starts Reborn in the background, writes its pid there, waits and removes it.
- **Restart 3** (the fix installed live): acknowledgement `Ready` 3.1 s after the request; the steps followed at
  once. Rebuild `out/y1-twentieth-boot-2`, BOOTIMG sha256
  `e2621cf1199142292da82b9edf12cab68b288916700a66a3279c06c270d13236` (`0x33a800` bytes).

## 15. Baseline tools and D3 build — 2026-11-15

Owner-approved ("do the baseline script and D3 build").

- **Drain baseline** (before D3 is flashed, so the two can be compared): `tools/development/y1-power-baseline.py
  start <label>` copies `tools/development/y1-power-log.sh` to `/data/power-baseline/` and starts it detached; it
  waits for the USB cable to be pulled, samples every 60 s (BAT0 voltage, the daemon's filtered voltage and
  percentage, the charger's unvalidated sense-resistor estimate, backlight, `/proc/stat`, cpu0 rate, wlan0) and
  stops when the cable returns, at 3.50 V or after the hour limit. `report` fetches the TSV and prints the
  voltage and percentage slopes, CPU busy share and screen-on share. Reads only; the one write is the TSV.
  Dry run on the owner's Y1 (fake USB flag, `/tmp`): every column filled.
- **Kernel**: `kernel/patches-y1/0042-mt6572-cpufreq.patch` adds an `mediatek,mt6572` entry to mainline's
  `mediatek-cpufreq` (1.15 V ceiling, no SRAM rail, no CCI); the fragment gains `CPU_FREQ`, `CPU_FREQ_STAT`, the
  schedutil default and the performance/powersave/userspace governors; `CPUFREQ_DT` stays off.
- **Deviations from §6**: (1) the intermediate parent is the **26 MHz crystal** (`cpu_mux` parent 0), not the fork's
  `mpll_d2`: MAINPLL's rate depends on the DRAM type and was never read on the Y1, so its half could exceed
  1001 MHz. (2) ARMPLL gets its own post-divider table (`clk-mt6572-apmixedsys.c`, `y1_armpll_div_table`): with the
  shared table clk-pll would relock 1001 MHz at /2 with a 2002 MHz VCO; now 1001 MHz is /1 (PCW `0x9a000`, LK's
  value), 806 and 598 MHz /2 (VCO 1612/1196 MHz, PCW `0xf8000`/`0xb8000`, exact). WHPLL (GPU) keeps the shared
  table. (3) the fork's OPP table (with `opp-supported-hw` but no speed-bin reader, which would drop every OPP) is
  not used: `y1_cpu_opp_table` lists 598/806/1001 MHz at 1.15 V. (4) VPROC is a `regulator-fixed` "vproc" at
  1.15 V (always-on, not boot-on: the packaging check forbids it), admitted in `y1_package.REGULATORS`.
- **RECOVERY** carries the same tree (the packaging invariant: it differs only in bootargs and write windows). The
  D3 test writes **BOOTIMG only**, so the installed RECOVERY stays the fallback at LK's fixed rate.
- Build `out/y1-boot-d3` (Y2ROOT `out/y1-root-asound`), BOOTIMG sha256
  `c3af5551b9e2e0ee17fca391b071ba1b896a2c2e986a89a5d97539f6180fe5b8`, RECOVERY
  `bc50a248b661ca6a75b246b68ff0f1fab4cea2a94976b7feec719dcf580f6ab3`. Not flashed.
- **Test** (`tools/development/y1-cpufreq-check.sh`, over SSH): driver `mtk-cpufreq` on policy0 with CPUs 0–1; the
  three rates set with `userspace`, each read back from sysfs and decoded from `ARMPLL_CON1` with the mux on
  ARMPLL, a fixed shell loop timed at each; optional soak (both cores busy, governor alternating, 10 minutes);
  ends on schedutil. Then charging, display, audio, Wi-Fi and SSH as usual, and a drain run with the same label
  as the baseline.

First idle run (old kernel, 2026-11-15, `out/power-baseline/idle-screenoff-wifi-on-bt-on-20261115-125559.tsv`):
started at 3.63 V, already at the daemon's 2 %, and hit the 3.50 V stop after 0.72 h (-190 mV/h in the cell's
tail). Not usable as a drain baseline; the one number that stands is idle CPU busy at 2.9 % (both cores,
screen off, Wi-Fi and BT on). The player then ran on until it went flat and cold-booted on replug.
`start` now refuses below 3.9 V without `--force`. Rerun from a full charge.

D3 on the player (2026-11-15, BOOTIMG `c3af5551…` written with mtkclient, RECOVERY unchanged):
`y1-cpufreq-check.sh` with a 10-minute soak **PASS**. mtk-cpufreq binds both cores; 598/806/1001 MHz each set,
read back and decoded from ARMPLL_CON1 (1001 MHz `0x8009A000` at /1, 806 MHz `0x810F8000` and 598 MHz `0x810B8000`
at /2), mux 1 throughout. A fixed shell loop takes 6.78 / 8.47 / 11.42 s, in proportion to the rate. 1092 governor
transitions under load, no fault; schedutil settles at 598 MHz when idle. Charging, Wi-Fi, Bluetooth, audio card
and Reborn as before; the kernel log has only the known boot messages. The drain comparison was dropped (owner).

## 16. Idle states, D4–D7 — 2026-11-15

The owner chose idle states over VPROC scaling for D4 onward. Stock runs 598, 806 and 1001 MHz at 1150 mV on
every speed bin, so a lower running VPROC has no vendor basis (and the Y2 forbids unmeasured undervolting);
stock's power saving is in idle and sleep.

**Evidence (MediaTek's 3.4.5 GPL drop, `drsami/mt6572_kernel` `c4b775c`, `mediatek/platform/mt6572/kernel/core/`;
read, not built):**

- `mt_idle.c` tries MCDI, then deep idle (dpidle), then WFI. MCDI/SODI is compiled out (`SPM_MCDI_FUNC`
  commented, `spm_mcdi_init` not registered), so stock uses **WFI and dpidle only**.
- dpidle needs **exactly one online CPU** (CPU1 hotplugged out), a clock mask that blocks on the UARTs, USB,
  APDMA, I2C, PWM, display/camera/MDP CGs and others (MSDC, thermal and AUXADC excluded), and at least
  54 500 13-MHz ticks (about 4.2 ms) to the next timer. It moves AUD_INTBUS to 26 MHz, enables the PWRAP's
  clock, arms GPT4 as the wake timer (the arch timer stops), clones the GIC into CIRQ, loads the 493-word
  `spm_pcm_dpidle` firmware and powers CPU0 down in DORMANT mode (L1 flushed, L2 retained).
- The dpidle firmware selects PWRAP DVFS slot 2 (`AP_DVFS_CON_SET` `0x10006604`), **VPROC 1.05 V** (`0x220` =
  `0x38`), on entry and slot 1 (1.15 V) on exit. Stock programs the slots in `mt_cpufreq_pdrv_probe`: 0 =
  1.25 V, 1 = 1.15 V, 2 = 1.05 V, 6/7 = `TOP_CKPDN0` set/clear.
- Suspend uses the 576-word `spm_pcm_suspend` (CPU and infra off, 26 MHz off); VPROC then falls to **0.85 V** by
  the MT6323's own sleep mode (`0x222` `VPROC_VOSEL_SLEEP` `0x18`, `0x230[8]` `VSLEEP_EN`, `0x216[1]`,
  `0x120[4]` `SRCLKEN_HW_MODE`), not by DVFS.
- CPU1 power-off: `spm_mtcmos_ctrl_cpu1` through `FC1_PWR_CON` (`0x10006218`) with `CPU_FC1_L1_PDN` (`+0x264`),
  status bit 11 in `PWR_STATUS`/`PWR_STATUS_S` (`+0x60c`/`+0x610`); power-up through `BOOT_SLV_CFG`/`ADDR`
  (`0x10001400`).
- SPM: base `0x10006000`, key `0x0b16 << 16`, IRQs SPI 72/73; load and kick in `spm_init_pcm`/`spm_kick_pcm`.

**Measured on D3 (old tick):** idle with the screen off wakes CPU0 100 times a second (GPT `timer@10008000`)
and CPU1 100 times through broadcast IPIs, plus about 55 IPIs and 11 MUSB interrupts a second with USB
attached. `CONFIG_ARM_ARCH_TIMER` was off, inherited from the Y2's first-boot fragment with no Y1 reason; the
fork dtsi already has the `arm,armv7-timer` node at 13 MHz.

**Stages:**

- **D4 — tick.** Arch timer, `NO_HZ_IDLE`, `HIGH_RES_TIMERS` (as the Y2's production fragment); the GPT stays
  registered as the broadcast device D6 will need. Configuration only. Test: boots; `current_clocksource` and
  `/proc/timer_list` show `arch_sys_counter` and per-CPU `arch_sys_timer`; idle interrupt rate well below
  100/s per CPU; `sleep 10` takes 10 s against the host; audio, Wi-Fi, Bluetooth, charging and cpufreq as on D3.
  Build `out/y1-boot-d4` (Y2ROOT `out/y1-root-asound`), BOOTIMG sha256
  `8d310738e8e5af54284ce119e7fcc4b9d3c3bfd1efa181bc8e86fd1ee7c6a5dd`. The fragment's old
  `# CONFIG_HIGH_RES_TIMERS is not set` (kept only because the periodic tick was the qualified state) is gone;
  flash BOOTIMG only, as for D3.
- **D4 run (2026-11-15): boot loop.** After the logo the preloader enumerates, the kernel's USB gadget never
  does, and the watchdog restarts the player: an early hang. D3's BOOTIMG was written back. Cause (INFERRED, no
  console): the arch counter does not run. Mainline's `mediatek_timer_init` enables GPT6 to ungate the arch
  timer only on MT6589/MT7623/MT8135/MT8127, and the Y2 needed `kernel/platform/local-timer.c` (stock's
  `mt_gpt_init`/`ca7_timer` contract with a counter check and a GPT fallback) for the same reason. D4 needs the
  Y1's stock GPT6/arch-timer preparation and a checked fallback before it can run again.
- **D4 fix (0043).** Stock's `mt_gpt.c` defines `CONFIG_HAVE_SYSCNT` "used for CA7 system counter":
  `setup_syscnt()` runs GPT6 (`0x10008060`) free at 13 MHz, divider 1 (`CON` `0x31`, `CLK` `0`), exactly
  mainline's `GPT6_CON_MT65xx` write, whose SoC list lacks the MT6572; LK leaves GPT6 at `0` (read on D3).
  Overlay 0043 on `arch/arm/mach-mediatek/mediatek.c` does that write for `mediatek,mt6572` and then requires
  the arch counter to advance within a factor of two of GPT6 over 13 000 GPT6 ticks (about 1 ms, timed by GPT6
  since `udelay` is not calibrated yet); otherwise it sets the `arm,armv7-timer` node's status to `disabled` and
  the GPT stays the only timer. mainline's GPT driver uses only GPT1/GPT2.
- **D4a run** (arch timer, periodic tick; BOOTIMG `31c24586…`): boots; `mt6572: GPT6 13001 ticks, arch counter
  13000 ticks`; clocksource `arch_sys_counter`; per-CPU `arch_timer` interrupts, broadcast IPIs 0;
  `sleep 30` = 30.02 s on the player's uptime.
- **D4b run** (plus `NO_HZ_IDLE`, `HIGH_RES_TIMERS`; build `out/y1-boot-d4b`, BOOTIMG sha256
  `ce0d4d1c300040aa86c648f448aace8abf372b4a945bae176aae841f5d5c2bc4`): **PASS**. `hres_active` 1 on both CPUs;
  `y1-cpufreq-check.sh` PASS (loop 6.80 / 8.46 / 11.46 s); `sleep 60` = 60.01 s; charging, Wi-Fi, Bluetooth,
  audio card and Reborn as before; no lockup or clocksource warning. Screen off, USB attached and charging,
  2 min after boot: CPU0 102 and CPU1 46 timer interrupts/s (D3: 100 + 100 broadcast). CPU0 keeps a high rate
  because of its wakers, not the tick: the charger's worker (`kworker/u11:0-y2-charger`) about 75 context
  switches/s from its 1 s poll, `S45y1-mtp`'s 1 s shell loop about 17/s (some 15 forks per round), the power
  daemon about 11/s. Those are separate work (drivers and userspace), not D4.
- **D5 build (0044).** Overlay on `arch/arm/mach-mediatek/platsmp.c`, `CONFIG_HOTPLUG_CPU`. `cpu_die` on CPU1:
  stock's `cpu_enter_lowpower` as mainline's `v7_exit_coherency_flush(louis)` (SCTLR.C off, L1 clean and
  invalidate, ACTLR.SMP off), the flush called through a register because clang's assembler rejects the macro's
  `bl` here ("unsupported relocation type"); disassembly checked: no stack access between C off and the WFI
  loop. `cpu_kill` on CPU0: stock's `spm_mtcmos_ctrl_cpu1(STA_POWER_DOWN)` (APMCU1 WFI in `SLEEP_TIMER_STA`,
  then CLK_DIS, SRAM_CKISO, ISO, SRAM_ISOINT_B off, RST_B off, `CPU_FC1_L1_PDN` with ACK, PWR_ON, PWR_ON_S, FC1
  off in both status registers). Online again: `BOOT_SLV_CFG` `0x13800001`, `BOOT_SLV_ADDR` = the kernel's
  secondary entry, then the power-on sequence; the boot ROM jumps in. Every poll is bounded (10 ms); a CPU1
  left in an unknown state is not booted again. Build `out/y1-boot-d5`, BOOTIMG sha256
  `527271ac81747c28dcecf3751ad0cd21057f1395181b80efb33d8c8bea67209b`.
- **D5 run (2026-11-15): PASS for the mechanism.** Offline: `FC1_PWR_CON` `0x4d` -> `0x32`, `PWR_STATUS` and
  `PWR_STATUS_S` `0x3fff` -> `0x37ff` (bit 11 off), `CPU_FC1_L1_PDN` `0x101`; online restores `0x4d`/`0x3fff`/`0`.
  200 idle cycles in 23.6 s and 100 cycles with both CPUs spinning while the userspace governor stepped
  598/1001/806 MHz every 0.3 s: 0 failures, no kernel message. One minute with CPU1 off: Wi-Fi pings, Reborn
  runs, CPU0 112 timer interrupts/s. Afterwards cpufreq check PASS, charging, Bluetooth and the audio card as
  before. The policy was open at this point.
- **D5b policy (owner's choice: the screen-off rule).** The backlight driver (`kernel/platform/backlight.c`, shared
  with the Y2 but inert there) takes the CPU named by the DT property `innioasis,screen-off-offline-cpu` offline
  through `remove_cpu()` when the screen goes dark (`bl_power` or brightness 0), and brings it back through
  `add_cpu()` when it lights, from a work item on `system_unbound_wq`. The Y1 DTS sets `<1>`. Build
  `out/y1-boot-d5b`, BOOTIMG sha256 `64cbc6440e03cfd775316850fbbebc9d0ef00595b7ab478852332253a7eb3abe`.
- **D5b run (2026-11-16): PASS.** `bl_power` 4/0 by hand, five times: `online` `0` with `PWR_STATUS` `0x37ff` and
  `FC1_PWR_CON` `0x32`, then `0-1` with `0x3fff`. Reborn's own screen timeout set `bl_power` 4 and CPU1 went
  offline within 5 s. 50 cycles with both CPUs spinning: 0 failures, no kernel message. With CPU1 off, a
  3 s `aplay` ran without error. Afterwards cpufreq check PASS (1 min soak), Wi-Fi up, charging, `bluetoothd`
  and Reborn running.
- **D5 — CPU1 power-off.** `HOTPLUG_CPU` with the stock `FC1_PWR_CON` sequence (a `smp_operations` cpu_kill /
  boot path in the MT6572 platform code) and a policy that takes CPU1 offline when idle. Prerequisite for D6.
- **D6 — deep idle.** A cpuidle state on CPU0 with stock's entry conditions (one CPU online, the clock
  mask through CCF, residency), mainline `cpu_pm`/`cpu_suspend` for the context instead of stock's
  `mt_dormant.c`, the dpidle firmware as data, CIRQ, GPT wake, and the PWRAP DVFS slots 1/2 with readback.
  New admission needed: SPM, PWRAP DVFS slot and `AP_DVFS_CON_SET` writes.
  Owner's decision (2026-11-16): D6 ports the Y2's coordinator (`kernel/platform/system-idle.c`: 30 s quiet
  window, playback/scan/transfer leases, input and display restore, escalating hold) to the Y1 and retires the
  D5b backlight hook. Its Y2-only inputs (`y2_cpu_safe`, CCF reclaim, local timer, boot mode) need Y1 owners.
- **D7 — suspend.** The suspend firmware and the MT6323 sleep-mode registers above; only after D6.

## 17. D6 admission — 2026-11-16

**Status: admitted by the owner (2026-11-16), all four steps D6a–D6d, within the bounds below.**
The owner decided (§16) that D6 ports the Y2's coordinator. D6 is split so that each step adds one kind of
write, and the risky parts start disabled.

**Read on the player (D5b, 2026-11-16, reads only):**

- PWRAP DVFS slots `0x1000f0e4`–`0x1000f120` (`DVFS_ADR0`/`WDATA0` … `ADR7`/`WDATA7`): **all 0**. LK does not
  program them and mainline has no owner, so D6 must write them before the SPM may request a voltage.
- `SPM_AP_DVFS_CON_SET` `0x10006604` = `0x80000007`: slot 7 is selected, with every slot empty.
- SPM `POWERON_CONFIG_SET` `0x10006000` = `1`; PCM not loaded; `PWR_STATUS`/`_S` `0x3fff` (CPU1 on).
- GPT4 (`0x10008040`) `CON` = `0x31`, counting (`0x3996f3e3`). Mainline's GPT driver uses GPT1/GPT2 only, so GPT4
  was started before Linux (INFERRED: LK). Stock's dpidle takes GPT4 as its one-shot wake timer.
- `CA7_CACHE_CONFIG` `0x10200000` = `0x20`. Stock sets bit 4 (keep L2 on reset) for dormant and clears it
  afterwards.
- No cpuidle driver is registered; idle is the architecture's plain WFI.

**Stock values D6 uses** (`drsami/mt6572_kernel` `c4b775c`):

- `mt_cpufreq_pdrv_probe`, the live table (the `ptpod` alternative is commented out):
  - `ADR0`–`ADR5` = `0x220` (`VPROC_VOSEL_ON`); `ADR6`/`ADR7` = `TOP_CKPDN0` set/clear.
  - `WDATA0` = `0x58` (1.25 V), `WDATA1` = `0x48` (1.15 V), `WDATA2` = `0x38` (1.05 V, "for spm firmware"),
    `WDATA3`–`5` = 1.15 V plus the speed-bin variation, `WDATA6`/`7` = `0x2`.
- dpidle (`mt_spm_dpidle.c`, `mt_idle.c`):
  - The 493-word `spm_pcm_dpidle` firmware, `pwrlevel` 0 (UART sleep handshake on), `infra_pdn` false,
    `lock_infra_dcm`, and the wake sources KP, GPT, EINT, CCIF, USB_CD/PDN, AFE, SYSPWREQ, MD/CONN WDT,
    CONN and THERM.
  - `AUD_INTBUS` is moved to 26 MHz and `PMIC_SW_CG_AP` enabled ("SPM need PMIC wrapper clock").
  - GPT4 is set one-shot to the next timer; the minimum is 54 500 13-MHz ticks.
  - The GIC is cloned into CIRQ.
  - CPU0 enters `DORMANT_MODE` with `BOOT_SLV_CFG` = `0x13800001` and `BOOT_SLV_ADDR` = the resume entry.
  - `CA7_CACHE_CONFIG` bit 4 is set.
- The firmware selects slot 2 on entry and slot 1 on exit (§16). Stock's own PWRAP stress test checks each
  request by reading `0x220` back (`0x38`, then `0x48`).

**What is ported from the Y2:**
- `system-idle-policy.h`, the pure decision logic, is ported unchanged.
- `system-idle.c`'s sampling and park/restore loop is ported with Y1 inputs: the backlight's `screen_dark`, a
  playback lease from the ALSA PCM state, and no Y2 boot or CPU-safety modes. It replaces the D5b backlight
  hook.
- The ordering and fallbacks of `idle.c` and `spm.c`'s `y2_spm_dormant_idle`: `cpu_pm_enter`,
  `cpu_cluster_pm_enter`, `cpu_suspend`, the readback of every arm write, restore on every path, and a
  `broken` latch.
- The MT6582 constants (479-word firmware, event vectors, `CON` bits) are **not** reused. Every MT6572 value
  comes from the stock files above.

| Step | What | Writes (beyond D5) | Starts |
| --- | --- | --- | --- |
| **D6a** | Coordinator port: CPU1 parks after the Y2's 30 s quiet window with the screen dark and no playback; input, display or load restores it | none (Linux hotplug through D5's path) | enabled |
| **D6b** | Program the PWRAP DVFS slots with stock's table; prove the handshake with **slot 1 only** (1.15 V, today's voltage) | `DVFS_ADR0`–`7`/`WDATA0`–`7` once at boot; `AP_DVFS_CON_SET` low 3 bits = 1 | enabled (one request at boot) |
| **D6c** | cpuidle state 1 on CPU0: dpidle firmware with `cpu_pdn` = 0 (CPU0 keeps its state in WFI; the firmware lowers VPROC to 1.05 V and raises it on wake) | SPM PCM load/kick/clean as stock; GPT4 one-shot; CIRQ; `AUD_INTBUS` mux; `PMIC_SW_CG_AP` | **disabled**; enabled by hand through sysfs |
| **D6d** | The same with `cpu_pdn` = 1: CPU0 dormant, L2 retained, resume through the boot ROM | `BOOT_SLV_CFG`/`ADDR` for CPU0, `CA7_CACHE_CONFIG` bit 4 | **disabled**; enabled by hand through sysfs |

**Bounds:**

- **PWRAP slots:**
  - Only stock's table, compiled in as constants.
  - Each `ADR`/`WDATA` is written and read back before any request. A mismatch leaves deep idle off for the
    boot.
  - Only slots 1 and 2 may be selected at all.
  - Linux itself only ever requests slot 1, which is today's 1.15 V, and checks `0x220` reads `0x48`.
  - Slot 2 (1.05 V) is requested only by stock's firmware while CPU0 is idle.
  - No direct PMIC write to `0x220`, no other VPROC value, no VCORE slot use.
- **Entry conditions:** stock's, enforced in C.
  - CPU0 is the only CPU online (D6a).
  - The screen is dark and nothing is playing.
  - cpufreq at or below 1001 MHz, at 1.15 V.
  - At least 54 500 GPT ticks to the next timer.
  - Stock's clock-mask blockers (UARTs, USB, APDMA, I2C, PWM, display/MDP and others) are read from the CG
    registers as reads.
- **USB:** with the gadget connected, the USB clock blocks entry exactly as in stock. The D6c/D6d tests
  therefore run over Wi-Fi SSH with USB unplugged.
- **Failure latch:** any arm readback mismatch, a non-zero PCM debug register, a CIRQ or timer restore fault,
  or a resume through the wrong path sets `broken`. The CPU then stays in plain WFI until reboot.
- **GPT4:** taken by the idle driver only after a check at probe that no Linux driver owns it; its prior
  `CON`/`CLK` are recorded and restored on remove.
- **Off by default:** D6c and D6d start disabled. A hang or reset reboots into the same image with deep idle
  off, so recovery needs no reflash. The RGU watchdog already resets a hung player (§7).

**Risks:**

- **A wrong slot address could write another PMIC register.**
  - Guarded by the constant table and readback.
  - Slot 7's `TOP_CKPDN0_CLR` with data `0x2` is stock's own value.
- **Resuming at 1.05 V and 1001 MHz could crash CPU0** if the exit request did not complete.
  - Stock's firmware waits for the PWRAP acknowledge.
  - D6c tests this with the CPU powered, before D6d powers it off.
  - INFERRED until the D6c run.
- **The UART sleep handshake** (`pwrlevel` 0) is the step that broke the Y2's C3 at first.
  - The Y1 has no console on UART, but the SPM still asks.
  - D6c records the acknowledge count.
- **The arch timer stops in dormant.**
  - The state carries `CPUIDLE_FLAG_TIMER_STOP`, so the GPT broadcast from D4 takes over.

**Tests:**

- **D6a:**
  - CPU1 parks within about 35 s of the screen going dark.
  - Input, display wake or a playback start restores it before acknowledging.
  - Reborn's timeout drives it.
  - D5's health checks pass.
- **D6b:**
  - The boot log shows all 16 slot readbacks matching and the slot 1 request reading `0x220` = `0x48`.
  - `AP_DVFS_CON_SET` reads `…1`.
- **D6c, then D6d, each over Wi-Fi with USB unplugged:**
  - **Enable through sysfs:**
    - entries and residency rise;
    - wake reasons are recorded;
    - the restore-failure count stays 0.
  - **Timing:** `sleep 60` takes 60 s against the host.
  - **Workload:**
    - Wi-Fi ping holds over 10 min;
    - a button press wakes the screen;
    - charging state still updates on replug.
  - **After the run:** the cpufreq check passes.
  - **Soak:** 1 h screen-off with no `broken`.
- **Drain (all steps):** after D6d, `y1-power-baseline.py` from 3.9 V or more, compared with D3's screen-off
  run.

**D6a build.** `kernel/y1/system-idle.c` (added as `drivers/soc/mediatek/y1-system-idle.c`,
`CONFIG_INNIOASIS_Y1_IDLE`, overlays 0045/0046): the Y2's coordinator with the Y2's unchanged
`system-idle-policy.h`. Its inputs are:
- the backlight's dark state;
- a stream lease from the Y1 AFE's trigger (bit 0 playback, bit 1 FM/capture), which is atomic: setting it
  queues a restore;
- input events;
- cpufreq transitions.

Two inputs are fixed: the Y2's timer handoff is 1, since D4's arch timer and GPT broadcast need none, and the
Y2's boot and CPU-safety modes do not exist on the Y1. The D5b backlight hook and its DTS property are gone,
and `backlight.c` is back to its pre-D5 code except that `y2_backlight_dark` is also built for the Y1. The
Y2's `system-idle.h` gains the Y1 symbol and the lease stub, so the Y2 build is unchanged. The policy's
constants are the Y2's: 400 milli-cores quiet is 20 % of the Y1's two cores. Build `out/y1-boot-d6a`,
BOOTIMG sha256 `0da782fafffdf1ba242107d0b7d9a4f12ba6719dbe1604973128f2c2b98d180a`.

**D6a run (2026-11-16): PASS.**
- **Parking:** with the screen dark, CPU1 parked after a 31 s quiet window (`FC1_PWR_CON` `0x32`,
  `PWR_STATUS` `0x37ff`).
- **Screen on:** `bl_power` 0 restored CPU1 before the backlight write.
- **Re-parking:** dark again, CPU1 parked after the 60 s hold plus 30 s quiet (93 s).
- **Playback:** `aplay` restored CPU1 at once (`leases=0x1`) and held it online for 45 s of dark playback;
  the lease cleared on stop.
- **Button:** a button press restored CPU1. The input and display paths were not separated within the
  watcher's 50 ms.
- **Health checks:** cpufreq check PASS; Reborn, `bluetoothd`, the audio card and charging fine; no kernel
  warning.
- **USB:** the cable was off the host for part of the run, which went over Wi-Fi.

**D6b build.** `kernel/y1/deep-idle.c` (added as `drivers/soc/mediatek/y1-deep-idle.c`,
`CONFIG_INNIOASIS_Y1_DEEP_IDLE`): it writes stock's slot table and reads it back, then requests slot 1 as
stock's `spm_dvfs_ctrl_volt` does and reads `VOSEL_ON` `0x220` and `NI_VPROC_VOSEL` `0x224` back. Two
preconditions come first: `VOSEL_ON` must already read `0x48`, and the wrapper's arbiter must have
`DVFSINF` (bit 4) enabled; it reads `0x7f` on the Y1, like the Y2's implemented mask.

The first image ran from `late_initcall` before the PMIC wrapper's deferred probe (MT6323 at 0.683 s, the
call at 0.678 s). It stopped at stage `pmic` and wrote nothing (slots 0, selector `0x80000007`
unchanged), as the guard intends. The table is now written from a work item that retries every 200 ms
for up to 10 s. Build `out/y1-boot-d6b`, BOOTIMG sha256 `d10a6732cb8e8e7f8a33a04e28b39636578ce8652a63c7af47be3252974b9e69`.

**D6b run (2026-11-16): PASS.**
- **Boot log:** `DVFS slots ready, slot 1 acknowledged after 0 polls, VPROC 0x48/0x48` at 0.55 s, on the
  first try.
- **Slot readback:** `0x1000f0e4`–`0x1000f120` hold `0x220/0x58`, `0x220/0x48`, `0x220/0x38`, then
  `0x220/0x48` for slots 3–5, `0x104/0x2` and `0x106/0x2`.
- **Selector:** `AP_DVFS_CON_SET` reads `0x80000001`.
- **Health checks:** cpufreq check PASS; Wi-Fi, Reborn, `bluetoothd` and `aplay` fine; charging; no kernel
  warning.
- **Limit:** the acknowledge bit was already set from LK and read set on the first poll, and 1.15 V is
  today's value. The request is therefore shown to do no harm, not shown to have been performed. D6c's
  wake path, slot 2 then slot 1, is the first observable voltage change.

