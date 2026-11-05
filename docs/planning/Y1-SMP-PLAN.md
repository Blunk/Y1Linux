# Y1 second CPU core (SMP)

Status: **PASS** on the owner's player (2026-11-05, [run](#7-run--pass--2026-11-05)): both Cortex-A7 cores run;
approved by the owner the same day. Every Y1 kernel so
far was built without SMP (`# CONFIG_SMP is not set`; boot log "DT /cpu 2 nodes greater than max cores
1"), so the MT6572's second Cortex-A7 is idle. The owner asked for both cores.

## 1. Evidence

- **Mainline 6.18** already supports the MT6572 here (`arch/arm/mach-mediatek/platsmp.c`,
  `mtk_mt6572_boot`): it writes the secondary entry point to `0x10001408` and the key `0x534c4131` to
  `0x1000140c`, then sends a wake-up IPI. The fork's `mt6572.dtsi` declares both CPUs with
  `enable-method = "mediatek,mt6589-smp"`.
- **Stock 3.0.2** (`boot_secondary` 0xc05955a8): the same key at `0x1000140c`, writes to `0x10001400`/`0x404`,
  and `spm_mtcmos_ctrl_cpu1(1)` (0xc0030324), which powers CPU1 up through the SPM (key `0x0b160001`,
  `CPU1_PWR_CON` `0x10006218` power-on, wait for `PWR_STATUS` bit 11, L1 power-down release at
  `0x10006264`, de-isolate, clock, reset release). The Y2's SMP needed the same step (`0006-mt6582-smp`,
  `y2_spm_cpu_boot`).
- **The owner's Y1 after LK** (read-only `devmem` over SSH, 2026-11-05): `CPU1_PWR_CON = 0x4d` (reset
  released, power on, isolation off, clock on: the end state of the stock power-up), `0x10006264 = 0`
  (L1 on), `PWR_STATUS`/`_2ND = 0x3fed` (bit 11 set), `0x10006720 = 0x00050000` (bit 16: CPU1 in standby,
  WFI), boot registers `0x10001400`–`0x1000140c` all 0. **LK leaves CPU1 powered and waiting in the boot
  ROM**, so mainline's path is enough: no SPM write.

## 2. Design (twelfth boot)

- Kernel: the eleventh fragment plus `SMP`, `NR_CPUS=2` (and `SMP_ON_UP` off). Without per-CPU timers
  (the ARM architected timer stays off, as in every Y1 boot) the MT6572 GPT serves both CPUs through the
  clock-event broadcast that ARM SMP selects; the tick stays periodic.
- No CPU hotplug (mainline's MT6589-style SMP ops have no `cpu_die`), no `cpufreq`, no CPU idle.
- Everything else as the eleventh boot.

## 3. Risks

If CPU1 does not answer, the kernel reports "CPU1: failed to come online" and continues on CPU0 (no
writes beyond the two boot registers). Concurrency bugs in drivers that were only ever run on one CPU
(the Y1's own drivers, the shared Y2 charger, ADC and backlight are SMP-qualified on the Y2's four cores).

## 4. Test

`nproc` and `/proc/cpuinfo` show two processors; the boot log has "Brought up 1 node, 2 CPUs";
`/proc/interrupts` has a column per CPU; a CPU-bound job on each core (`yes > /dev/null &` twice) for a
minute shows both busy in `top`; charging, display, SSH and the wheel still work; no new kernel warning.

## 5. Admission

| Item | Bound |
| --- | --- |
| **Start the second CPU core** | mainline's MT6572 path only: the entry address and key in the boot registers `0x10001408`/`0x1000140c` and an IPI; no SPM power write (LK already powered CPU1); no hotplug, cpufreq or idle states |

## 6. Build — 2026-11-05

`kernel/config/y1-twelfth-boot.config`: the eleventh fragment with `SMP`, `NR_CPUS=2`, `SMP_ON_UP` and
`HOTPLUG_CPU` off (resolved: `GENERIC_CLOCKEVENTS_BROADCAST`); `innioasis-y1-twelfth-boot.dts`: the eleventh
tree, new model string; PID1 and initramfs as the eleventh. Build `out/y1-twelfth-boot-1`, BOOTIMG sha256
`26f6d4731aa9dbadc2790942e48ea5c7562ae01bdac1b983a7a3b5870edc801b` (`0x308800` bytes).

## 7. Run — PASS — 2026-11-05

Build `26f6d473…`; `evidence-private/2026-11-05-y1-smp/run1.txt`; the owner saw both cores. Over SSH:
`nproc` 2, two processors in `/proc/cpuinfo`; `smp: Brought up 1 node, 2 CPUs`, `Total of 2 processors
activated (3994.41 BogoMIPS)`, no warning (the MT6323 `of_node` notices are the known ones); `/proc/interrupts`
has a column per CPU (all interrupts on CPU0, as without balancing). Two `yes` loops for 20 s: total CPU 100 %
user, `/proc/stat` cpu0 2038 and cpu1 2228 user ticks. Charging (`BAT0` Charging), the backlight (32), both
input devices and SSH unaffected. **Second core: PASS.**
