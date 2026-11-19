#!/bin/sh
# D6c/D6d run on the Innioasis Y1 (docs/planning/Y1-POWER-PLAN.md §17). Deep idle needs both radios off (B2), so it
# cannot be watched over Wi-Fi: this runs detached on the player and logs to /data/logs/y1-dpidle-run.log.
#   scp-less copy:  ssh root@<player> 'cat > /tmp/y1-dpidle-run.sh' < this
#   start:          ssh root@<player> 'setsid sh /tmp/y1-dpidle-run.sh 10 [stanby ...] </dev/null >/dev/null 2>&1 &'
#   D6d (state2, CPU0 powered down): Y1_DPIDLE_STATE=2 setsid sh /tmp/y1-dpidle-run.sh ...
# Switches Bluetooth and Wi-Fi off through Reborn, waits for the USB cable to come out (up to 10 min), enables
# cpuidle state1 (DPIDLE), or state2 (DORMANT) with Y1_DPIDLE_STATE=2, samples once a minute for the given
# minutes with the screen left to go dark, then disables the state and switches both radios on. With stanby
# values (dpidle_stanby, the AP_STANBY_CON request masks), it runs the given minutes once per value and puts the
# stock value back at the end.
# The Y1 has no RTC device: compare the player's uptime with the host's clock before and after the run.
# SPDX-License-Identifier: GPL-2.0-only
set -u
minutes=${1:-10}
state=state${Y1_DPIDLE_STATE:-1}
shift
p0=/sys/module/y1_deep_idle/parameters/dpidle_stanby
stock=$(cat $p0)
values=${*:-$stock}
log=/data/logs/y1-dpidle-run.log
p=/sys/module/y1_deep_idle/parameters
st=/sys/devices/system/cpu/cpu0/cpuidle
exec >"$log" 2>&1
snap() {
    echo "== $1 uptime=$(cut -d' ' -f1 /proc/uptime)"
    for f in dpidle_broken dpidle_stage dpidle_entries dpidle_normal dpidle_aborts dpidle_unknown \
        dpidle_gpt_wakes dpidle_uart_acks dpidle_uart_timeouts dpidle_cirq_flushed dpidle_refusals \
        dpidle_last_wake dpidle_last_event dpidle_last_r13 dpidle_last_dvfs dpidle_last_debug dpidle_last_fsm \
        dpidle_stanby dpidle_last_timer_out dpidle_timer_ticks dpidle_last_isr dpidle_r12_26m_slp dpidle_r12_ap_sleep \
        dpidle_dormant_entries dpidle_dormant_resumes dpidle_dormant_wfi_returns dpidle_dormant_last; do
        echo "$f=$(cat $p/$f)"
    done
    echo "state0 usage=$(cat $st/state0/usage) $state usage=$(cat $st/$state/usage) time_us=$(cat $st/$state/time)" \
        "disable=$(cat $st/$state/disable) online=$(cat /sys/devices/system/cpu/online)"
    echo "ctrl0=$(devmem 0x10000020) ctrl1=$(devmem 0x10000024) mm0=$(devmem 0x14000100) mm1=$(devmem 0x14000110)" \
        "dvfs=$(devmem 0x10006604) bl_power=$(cat /sys/class/backlight/y2-backlight/bl_power)"
}
[ -d "$st/$state" ] || { echo "FAIL: no cpuidle $state"; exit 1; }
echo "boot: spm=$(cat $p/dpidle_spm_boot) gpt4=$(cat $p/gpt4_boot) driver=$(cat /sys/devices/system/cpu/cpuidle/current_driver)"
up0=$(cut -d' ' -f1 /proc/uptime)
snap start
rebornctl bluetooth off; rebornctl wifi off
w=0
while [ "$(cat /sys/class/power_supply/y2-usb-presence/online)" = 1 ] && [ $w -lt 600 ]; do sleep 5; w=$((w + 5)); done
snap "cable out after ${w}s"
echo 0 > $st/$state/disable
snap enabled
for v in $values; do
    echo "$v" > $p0
    a0=$(cat $p/dpidle_aborts); n0=$(cat $p/dpidle_normal); u0=$(cat $p/dpidle_unknown)
    k0=$(cat $p/dpidle_timer_ticks); t0=$(cat $st/$state/time); s0=$(cat $p/dpidle_r12_26m_slp)
    r0=$(cat $p/dpidle_dormant_resumes); w0=$(cat $p/dpidle_dormant_wfi_returns)
    snap "stanby $v"
    n=0
    while [ $n -lt "$minutes" ]; do
        sleep 60
        n=$((n + 1))
        snap "stanby $v minute $n"
        [ "$(cat $p/dpidle_broken)" = Y ] && break
    done
    echo "RESULT stanby=$v aborts=$(($(cat $p/dpidle_aborts) - a0)) normal=$(($(cat $p/dpidle_normal) - n0))" \
        "unknown=$(($(cat $p/dpidle_unknown) - u0)) last_wake=$(cat $p/dpidle_last_wake) last_event=$(cat $p/dpidle_last_event)" \
        "ticks32k=$(($(cat $p/dpidle_timer_ticks) - k0)) state_us=$(($(cat $st/$state/time) - t0))" \
        "r12_26m_slp=$(($(cat $p/dpidle_r12_26m_slp) - s0)) dormant_resumes=$(($(cat $p/dpidle_dormant_resumes) - r0))" \
        "dormant_wfi_returns=$(($(cat $p/dpidle_dormant_wfi_returns) - w0))"
    [ "$(cat $p/dpidle_broken)" = Y ] && { echo 'BROKEN: stopping early'; break; }
done
echo 1 > $st/$state/disable
echo "$stock" > $p0
up1=$(cut -d' ' -f1 /proc/uptime)
snap end
echo "elapsed monotonic=$(echo "$up0 $up1" | awk '{printf "%.1f", $2 - $1}') s"
rebornctl bluetooth on; rebornctl wifi on
echo '== dmesg tail'
dmesg | tail -40
echo DONE
