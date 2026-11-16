#!/bin/sh
# D6c run on the Innioasis Y1 (docs/planning/Y1-POWER-PLAN.md §17). Deep idle needs both radios off (B2), so it
# cannot be watched over Wi-Fi: this runs detached on the player and logs to /data/logs/y1-dpidle-run.log.
#   scp-less copy:  ssh root@<player> 'cat > /tmp/y1-dpidle-run.sh' < this
#   start:          ssh root@<player> 'setsid sh /tmp/y1-dpidle-run.sh 10 </dev/null >/dev/null 2>&1 &'
# Switches Bluetooth and Wi-Fi off through Reborn, enables cpuidle state1 (DPIDLE), samples once a minute for
# the given minutes with the screen left to go dark, then disables the state and switches both radios on.
# The Y1 has no RTC device: compare the player's uptime with the host's clock before and after the run.
# SPDX-License-Identifier: GPL-2.0-only
set -u
minutes=${1:-10}
log=/data/logs/y1-dpidle-run.log
p=/sys/module/y1_deep_idle/parameters
st=/sys/devices/system/cpu/cpu0/cpuidle
exec >"$log" 2>&1
snap() {
    echo "== $1 uptime=$(cut -d' ' -f1 /proc/uptime)"
    for f in dpidle_broken dpidle_stage dpidle_entries dpidle_normal dpidle_aborts dpidle_unknown \
        dpidle_gpt_wakes dpidle_uart_acks dpidle_uart_timeouts dpidle_cirq_flushed dpidle_refusals \
        dpidle_last_wake dpidle_last_event dpidle_last_r13 dpidle_last_dvfs dpidle_last_debug dpidle_last_fsm; do
        echo "$f=$(cat $p/$f)"
    done
    echo "state0 usage=$(cat $st/state0/usage) state1 usage=$(cat $st/state1/usage) time_us=$(cat $st/state1/time)" \
        "disable=$(cat $st/state1/disable) online=$(cat /sys/devices/system/cpu/online)"
    echo "ctrl0=$(devmem 0x10000020) ctrl1=$(devmem 0x10000024) mm0=$(devmem 0x14000100) mm1=$(devmem 0x14000110)" \
        "dvfs=$(devmem 0x10006604) bl_power=$(cat /sys/class/backlight/y2-backlight/bl_power)"
}
[ -d "$st/state1" ] || { echo 'FAIL: no cpuidle state1 (DPIDLE not registered)'; exit 1; }
echo "boot: spm=$(cat $p/dpidle_spm_boot) gpt4=$(cat $p/gpt4_boot) driver=$(cat /sys/devices/system/cpu/cpuidle/current_driver)"
up0=$(cut -d' ' -f1 /proc/uptime)
snap start
rebornctl bluetooth off; rebornctl wifi off
sleep 5
echo 0 > $st/state1/disable
snap enabled
n=0
while [ $n -lt "$minutes" ]; do
    sleep 60
    n=$((n + 1))
    snap "minute $n"
    [ "$(cat $p/dpidle_broken)" = Y ] && { echo 'BROKEN: stopping early'; break; }
done
echo 1 > $st/state1/disable
up1=$(cut -d' ' -f1 /proc/uptime)
snap end
echo "elapsed monotonic=$(echo "$up0 $up1" | awk '{printf "%.1f", $2 - $1}') s"
rebornctl bluetooth on; rebornctl wifi on
echo '== dmesg tail'
dmesg | tail -40
echo DONE
