#!/bin/sh
# Battery drain logger for the Innioasis Y1 (docs/planning/Y1-POWER-PLAN.md, baseline before D3 and later stages).
# Runs on the player, detached, started by tools/development/y1-power-baseline.py. Reads only: sysfs, /proc and
# the power daemon's /run/y2/power.json; writes one TSV under /data/power-baseline and nothing else.
#
#   y1-power-log.sh <label> <interval_s> <max_hours>
#
# Waits up to 10 minutes for the USB cable to be pulled (a charging pack says nothing about drain), then samples
# every <interval_s> until the cable comes back, the battery falls to 3.50 V (above the daemon's 3.4 V floor),
# or <max_hours> pass. The last line of <log>.done names the reason.
# SPDX-License-Identifier: GPL-2.0-only
set -u

label=${1:?label}
interval=${2:-60}
max_hours=${3:-24}
case "$label" in *[!A-Za-z0-9._-]*|'') echo 'label: letters, digits, . _ - only' >&2; exit 2;; esac
case "$interval$max_hours" in *[!0-9]*) echo 'interval and hours are whole numbers' >&2; exit 2;; esac
[ "$interval" -ge 10 ] && [ "$max_hours" -ge 1 ] || { echo 'interval >= 10 s, hours >= 1' >&2; exit 2; }

dir=/data/power-baseline
mkdir -p "$dir"
log="$dir/$label-$(date +%Y%m%d-%H%M%S).tsv"
bat=/sys/class/power_supply/BAT0
usb=/sys/class/power_supply/y2-usb-presence
cpu=/sys/devices/system/cpu/cpu0/cpufreq
min_uv=3500000

value() { cat "$1" 2>/dev/null || echo -; }
# A key=value field of BAT0's charging_state (sense_current_est_ua, pmic_die_mc, ...); '-' if absent.
state_field() {
    v=$(tr ' ' '\n' < "$bat/charging_state" 2>/dev/null | sed -n "s/^$1=//p" | head -n 1)
    echo "${v:--}"
}
# A top-level or battery number of the power daemon's record; '-' if absent or null.
power_field() {
    v=$(sed -n "s/.*\"$1\":\([0-9.]*\).*/\1/p" /run/y2/power.json 2>/dev/null | head -n 1)
    echo "${v:--}"
}
netstate() { value /sys/class/net/wlan0/operstate; }
finish() {
    echo "$1" > "$log.done"
    if [ -r "$cpu/stats/time_in_state" ]; then
        for c in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/stats/time_in_state; do
            echo "# time_in_state $c"; cat "$c"
        done >> "$log.done"
    fi
    exit 0
}

echo "$$" > "$dir/running.pid"
trap 'rm -f "$dir/running.pid"' EXIT
{
    echo "# label=$label interval_s=$interval max_hours=$max_hours kernel=$(uname -r)"
    echo "# governor=$(value "$cpu/scaling_governor") freqs=$(value "$cpu/scaling_available_frequencies")"
} > "$log"

waited=0
while [ "$(value "$usb/online")" = 1 ]; do
    [ "$waited" -lt 600 ] || finish 'usb never unplugged (waited 600 s)'
    sleep 5
    waited=$((waited + 5))
done

printf 'epoch\tuptime_s\tstatus\tusb\tvoltage_uv\tdaemon_filtered_uv\tdaemon_percent\tsense_ua\tsense_drop_uv\tpmic_die_mc\tbl_power\tbrightness\tcpu_user\tcpu_nice\tcpu_system\tcpu_idle\tcpu_iowait\tcpu_irq\tcpu_softirq\tcpu0_khz\twlan0\tload1\n' >> "$log"
end=$(( $(date +%s) + max_hours * 3600 ))
while :; do
    read -r _ user nice system idle iowait irq softirq _ < /proc/stat
    read -r uptime _ < /proc/uptime
    read -r load1 _ < /proc/loadavg
    uv=$(value "$bat/voltage_now")
    online=$(value "$usb/online")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$(date +%s)" "$uptime" "$(value "$bat/status" | tr ' ' _)" "$online" "$uv" \
        "$(power_field filtered_voltage_uv)" "$(power_field percent)" \
        "$(state_field sense_current_est_ua)" "$(state_field sense_drop_uv)" "$(state_field pmic_die_mc)" \
        "$(value /sys/class/backlight/y2-backlight/bl_power)" "$(value /sys/class/backlight/y2-backlight/brightness)" \
        "$user" "$nice" "$system" "$idle" "$iowait" "$irq" "$softirq" \
        "$(value "$cpu/scaling_cur_freq")" "$(netstate)" "$load1" >> "$log"
    [ "$online" = 1 ] && finish 'usb plugged in'
    case "$uv" in *[!0-9]*|'') ;; *) [ "$uv" -ge "$min_uv" ] || finish 'battery at 3.50 V';; esac
    [ "$(date +%s)" -lt "$end" ] || finish "max_hours ($max_hours h) reached"
    sleep "$interval"
done
