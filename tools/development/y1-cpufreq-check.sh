#!/bin/sh
# D3 acceptance on the Innioasis Y1 (docs/planning/Y1-POWER-PLAN.md §6). Run over SSH: ssh root@10.42.0.1 sh -s [soak_minutes] < this
# Reads the cpufreq sysfs and, read-only, the CPU mux (0x10001000 bits 3:2) and ARMPLL_CON1 (0x10205104); sets
# each rate with the userspace governor and times a fixed shell loop; with soak_minutes > 0 loads both cores
# while the governor alternates between schedutil and powersave/performance. Ends on schedutil.
# SPDX-License-Identifier: GPL-2.0-only
set -u
soak=${1:-0}
policy=/sys/devices/system/cpu/cpufreq/policy0
[ -d "$policy" ] || { echo 'FAIL: no cpufreq policy (driver not bound; dmesg | grep -i cpufreq)'; exit 1; }
echo "driver=$(cat $policy/scaling_driver) cpus=$(cat $policy/related_cpus) governor=$(cat $policy/scaling_governor)"
echo "available=$(cat $policy/scaling_available_frequencies)"
echo "governors=$(cat $policy/scaling_available_governors)"
[ "$(cat $policy/scaling_available_frequencies)" = '598000 806000 1001000 ' ] || [ "$(cat $policy/scaling_available_frequencies)" = '598000 806000 1001000' ] ||
    echo 'WARN: rates differ from the admitted 598/806/1001 MHz'

# PCW in kHz: 26 MHz * pcw / 2^14 / 2^postdiv.
decode() {
    con1=$(devmem 0x10205104)
    mux=$(( ($(devmem 0x10001000) >> 2) & 3 ))
    pcw=$(( con1 & 0x1fffff )); div=$(( (con1 >> 24) & 7 ))
    echo "mux=$mux con1=$con1 armpll_khz=$(( 26000 * pcw / 16384 / (1 << div) ))"
}
spin() {
    start=$(cut -d' ' -f1 /proc/uptime)
    i=0; while [ "$i" -lt 200000 ]; do i=$((i + 1)); done
    end=$(cut -d' ' -f1 /proc/uptime)
    echo "loop_s=$(echo "$start $end" | awk '{printf "%.2f", $2 - $1}')"
}

fail=0
echo userspace > $policy/scaling_governor
for khz in 1001000 806000 598000 806000 1001000; do
    echo "$khz" > $policy/scaling_setspeed
    cur=$(cat $policy/scaling_cur_freq)
    regs=$(decode)
    echo "set=$khz cur=$cur $regs $(spin)"
    [ "$cur" = "$khz" ] || fail=1
    case "$regs" in "mux=1 "*"armpll_khz=$khz") ;; *) fail=1;; esac
done

if [ "$soak" -gt 0 ]; then
    echo "soak: ${soak} min, both cores busy, governor switching every 20 s"
    for c in 1 2; do (while :; do :; done) & eval "spinner$c=\$!"; done
    t=0
    while [ "$t" -lt $((soak * 60)) ]; do
        for g in schedutil powersave schedutil performance; do
            echo "$g" > $policy/scaling_governor; sleep 5; t=$((t + 5))
        done
        echo "t=${t}s cur=$(cat $policy/scaling_cur_freq) $(decode) battery_uv=$(cat /sys/class/power_supply/BAT0/voltage_now)"
    done
    kill "$spinner1" "$spinner2"
fi

echo schedutil > $policy/scaling_governor
echo "end governor=$(cat $policy/scaling_governor) cur=$(cat $policy/scaling_cur_freq)"
[ -r $policy/stats/time_in_state ] && { echo 'time_in_state:'; cat $policy/stats/time_in_state; }
echo "transitions=$(cat $policy/stats/total_trans 2>/dev/null)"
dmesg | grep -iE 'cpufreq|cpu[01]:' | tail -n 5
[ "$fail" = 0 ] && echo 'PASS: every rate set, read back and seen in ARMPLL' || echo 'FAIL: see the lines above'
