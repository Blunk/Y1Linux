/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 fifth-boot additions (docs/planning/Y1-CHARGING-PLAN.md, stage B), included after
 * y1-fourth-boot.h only when Y1_FIFTH_BOOT is defined, so the earlier PID1 binaries stay byte-identical.
 * The telemetry line gains the charger's own report: phase, programmed limit, fault bits and the
 * sense-current estimate from /sys/class/power_supply/BAT0/charging_state.
 */
static const char charging_state[] = "/sys/class/power_supply/BAT0/charging_state";

/* The value after " key=" (or at the start) in the charger's report, up to a space or newline. */
static void put_field(const char *report, long n, const char *key)
{
    unsigned length = 0;
    while (key[length]) ++length;
    for (long i = 0; i + (long)length < n; ++i) {
        if ((i && report[i - 1] != ' ' && report[i - 1] != '\n') || report[i + length] != '=') continue;
        unsigned j = 0;
        while (j < length && report[i + j] == key[j]) ++j;
        if (j < length) continue;
        char value[24];
        unsigned v = 0;
        for (long k = i + length + 1; k < n && report[k] != ' ' && report[k] != '\n' && v + 1 < sizeof(value); ++k)
            value[v++] = report[k];
        value[v] = 0;
        put(value);
        return;
    }
    put("?");
}

static void put_charge_state(void)
{
    static char report[2048];
    long n = read_text(charging_state, report, sizeof(report));
    put("\n  charge ");
    if (n <= 0) { put("unavailable"); return; }
    put_field(report, n, "phase"); put(" limit_ua "); put_field(report, n, "charge_limit_ua");
    put(" fault "); put_field(report, n, "fault");
    put(" est_ua "); put_field(report, n, "sense_current_est_ua");
    put(" budget_ua "); put_field(report, n, "input_budget_ua");
}
