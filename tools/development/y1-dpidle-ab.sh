#!/bin/sh
# D6c/D6d drain comparison on the Innioasis Y1 (docs/planning/Y1-POWER-PLAN.md §17): runs beside
# y1-power-log.sh (started by y1-power-baseline.py) and switches cpuidle state1 (DPIDLE), or state2 (DORMANT)
# with Y1_DPIDLE_STATE=2, on and off in equal blocks, so the logger's samples can be split by state under the
# same conditions and charge level.
#   copy:  ssh root@<player> 'cat > /data/power-baseline/y1-dpidle-ab.sh' < this
#   start: ssh root@<player> 'setsid sh /data/power-baseline/y1-dpidle-ab.sh <label> 1800 </dev/null >/dev/null 2>&1 &'
#   D6d:   ... 'Y1_DPIDLE_STATE=2 setsid sh /data/power-baseline/y1-dpidle-ab.sh ...'
# Switches Bluetooth and Wi-Fi off through Reborn (deep idle needs both off, B2), waits for the logger to start
# sampling (cable pulled), then alternates on/off every <block_s>, starting with on. One line per switch goes
# to /data/power-baseline/<label>-blocks.txt (epoch, state, the deep-idle counters). It ends when the logger
# ends (cable back in, 3.50 V or its hour limit), then leaves the state disabled and switches both radios on.
# SPDX-License-Identifier: GPL-2.0-only
set -u
label=${1:?label}
block=${2:-1800}
dir=/data/power-baseline
out="$dir/$label-blocks.txt"
p=/sys/module/y1_deep_idle/parameters
st=/sys/devices/system/cpu/cpu0/cpuidle/state${Y1_DPIDLE_STATE:-1}
alive() { [ -r "$dir/running.pid" ] && kill -0 "$(cat "$dir/running.pid")" 2>/dev/null; }
mark() {
    echo "$(date +%s) $1 entries=$(cat $p/dpidle_entries) aborts=$(cat $p/dpidle_aborts)" \
        "normal=$(cat $p/dpidle_normal) time_us=$(cat $st/time) broken=$(cat $p/dpidle_broken)" \
        "dormant_resumes=$(cat $p/dpidle_dormant_resumes) stanby=$(cat $p/dpidle_stanby)" \
        "refusals=$(cat $p/dpidle_refusals) online=$(cat /sys/devices/system/cpu/online)" >> "$out"
}
echo "# label=$label block_s=$block state=$(cat $st/name)" > "$out"
rebornctl bluetooth off >/dev/null; rebornctl wifi off >/dev/null
while alive && [ "$(cat /sys/class/power_supply/y2-usb-presence/online)" = 1 ]; do sleep 5; done
state=0
while alive; do
    echo $state > $st/disable
    mark "$([ $state = 0 ] && echo on || echo off)"
    t=0
    while alive && [ $t -lt "$block" ]; do sleep 10; t=$((t + 10)); done
    state=$((1 - state))
done
echo 1 > $st/disable
mark end
rebornctl bluetooth on >/dev/null; rebornctl wifi on >/dev/null
