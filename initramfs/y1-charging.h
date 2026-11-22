/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 PID1 charging observation (docs/planning/Y1-CHARGING-PLAN.md): a fixed list of MT6323 registers
 * through regmap debugfs (6.18 builds that file without a write path), the BATON pin as an estimated pack
 * temperature, and on the telemetry line the charger's own report: phase, programmed limit, fault bits and the
 * sense-current estimate from /sys/class/power_supply/BAT0/charging_state.
 */
enum { REGMAP_LINE = 11, REGMAP_STRIDE = 2 }; /* "rrrr: vvvv\n": 16-bit registers and values, stride 2 */

enum { PMIC_CHARGER_REGS = 0x24, PMIC_REGS = PMIC_CHARGER_REGS + 4, UNREAD = 0x10000 };

static char regmap_file[96]; /* /sys/kernel/debug/regmap/<pwrap>/registers, empty if absent */
static unsigned pmic_at_boot[PMIC_REGS]; /* taken before the session, normally without the cable */

/* 10 kOhm NTC, B 3435 (assumed: no part number), ohms from -20 C to 70 C in 5 C steps. */
static const unsigned ntc_ohms[] = {
    77523, 59606, 46290, 36290, 28704, 22897, 18410, 14916, 12171, 10000,
    8269, 6881, 5759, 4847, 4101, 3488, 2981, 2559, 2207,
};

/* Stock BattVoltToTemp's divider: 16.9 kOhm pull-up to 1.8 V, 27 kOhm in parallel with the NTC. Tenths
 * of a degree; 0 when the pin is out of the table's range. */
static int pack_tenths(unsigned pin_mv, int *tenths)
{
    unsigned parallel, ntc;
    if (pin_mv == 0 || pin_mv >= 1800) return 0;
    parallel = pin_mv * 16900 / (1800 - pin_mv);
    if (parallel >= 27000) return 0;
    ntc = parallel * 27000 / (27000 - parallel);
    for (unsigned i = 0; i + 1 < sizeof(ntc_ohms) / sizeof(ntc_ohms[0]); ++i)
        if (ntc <= ntc_ohms[i] && ntc >= ntc_ohms[i + 1]) {
            *tenths = (-20 + 5 * (int)i) * 10 + (int)((ntc_ohms[i] - ntc) * 50 / (ntc_ohms[i] - ntc_ohms[i + 1]));
            return 1;
        }
    return 0;
}

static void put_pack_temperature(void)
{
    int raw, tenths;
    put("  pack ");
    if (adc_raw("in_voltage5_raw", &raw) && pack_tenths(millivolts((unsigned)raw, 1800), &tenths)) {
        if (tenths < 0) { put("-"); tenths = -tenths; }
        put_number(tenths / 10); put("."); put_number(tenths % 10);
        put(" C?");
    } else {
        put("?");
    }
}

static int find_regmap(const char *dir, const struct dirent64 *entry)
{
    long length = 0;
    char device[80];
    while (entry->name[length]) ++length;
    if (!contains(entry->name, length, "pwrap")) return 0;
    copy(device, sizeof(device), dir, "/", entry->name);
    copy(regmap_file, sizeof(regmap_file), device, "/registers", 0);
    return 1;
}

static int hex_value(const char *p, unsigned digits, unsigned *value)
{
    *value = 0;
    for (unsigned i = 0; i < digits; ++i) {
        char c = p[i];
        unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 16;
        if (d == 16) return 0;
        *value = *value << 4 | d;
    }
    return 1;
}

/* One register by its line in the debugfs file; the line's own address must match. */
static int pmic_read(long fd, unsigned reg, unsigned *value)
{
    char line[REGMAP_LINE];
    unsigned address;
    long long position, offset = (long long)(reg / REGMAP_STRIDE) * REGMAP_LINE;
    if (call5(SYS_LLSEEK, fd, (long)(offset >> 32), (long)offset, (long)&position, SEEK_SET) != 0) return 0;
    if (call5(SYS_READ, fd, (long)line, REGMAP_LINE, 0, 0) != REGMAP_LINE) return 0;
    return hex_value(line, 4, &address) && address == reg && line[4] == ':' && hex_value(line + 6, 4, value);
}

static void put_hex(unsigned value, unsigned digits)
{
    char out[9];
    for (unsigned i = 0; i < digits; ++i) out[digits - 1 - i] = "0123456789abcdef"[(value >> (4 * i)) & 15];
    out[digits] = 0;
    put(out);
}

/* The charger block 0x000-0x046 (CHRDET is 0x000 bit 5), the chip ID, the boot OCV, the ADC control. */
static unsigned pmic_register(unsigned i)
{
    static const unsigned extra[] = { 0x100, 0x724, 0x758, 0x76e };
    return i < PMIC_CHARGER_REGS ? 2 * i : extra[i - PMIC_CHARGER_REGS];
}

/* Every listed register into values[], UNREAD where a line did not read back; 0 without the file. */
static int pmic_snapshot(unsigned values[PMIC_REGS])
{
    long fd = regmap_file[0] ? call5(SYS_OPEN, (long)regmap_file, O_RDONLY, 0, 0, 0) : -1;
    if (fd < 0) return 0;
    for (unsigned i = 0; i < PMIC_REGS; ++i)
        if (!pmic_read(fd, pmic_register(i), &values[i])) values[i] = UNREAD;
    call5(SYS_CLOSE, fd, 0, 0, 0, 0);
    return 1;
}

static void put_register_value(unsigned value)
{
    if (value == UNREAD) put("????");
    else put_hex(value, 4);
}

/* "reg=now/boot", the boot column taken before the session started (normally without the cable). */
static void shell_pmic(long watchdog)
{
    unsigned now[PMIC_REGS];
    if (!pmic_snapshot(now)) { usb_say("pmic: regmap debugfs unavailable\n"); return; }
    usb_say("register=now/at boot\n");
    for (unsigned i = 0; i < PMIC_REGS; ++i) {
        put_hex(pmic_register(i), 3); put("=");
        put_register_value(now[i]); put("/"); put_register_value(pmic_at_boot[i]);
        put((i + 1) % 5 && i + 1 < PMIC_REGS ? "  " : "\n");
        send(USB);
    }
    feed(watchdog);
}

static void charging_setup(void)
{
    if (call5(SYS_MOUNT, (long)"debugfs", (long)"/sys/kernel/debug", (long)"debugfs", MS_SILENT, 0) != 0)
        say("debugfs: mount failed\n");
    each_entry("/sys/kernel/debug/regmap", find_regmap);
    say(regmap_file[0] ? "pmic registers: " : "pmic registers: regmap debugfs not found\n");
    if (regmap_file[0]) { say(regmap_file); say("\n"); }
    for (unsigned i = 0; i < PMIC_REGS; ++i) pmic_at_boot[i] = UNREAD;
    if (pmic_snapshot(pmic_at_boot)) {
        put("pmic at boot: 0x000="); put_register_value(pmic_at_boot[0]);
        put(pmic_at_boot[0] != UNREAD && pmic_at_boot[0] & 0x20 ? " (charger present)\n" : " (no charger)\n");
        send(SCREEN);
    }
}

static int charging_command(const char *line, const char *argument, long watchdog)
{
    (void)argument;
    if (!same(line, "pmic")) return 0;
    shell_pmic(watchdog);
    return 1;
}

static int pack_temperature_selftest(void)
{
    int tenths;
    return pack_tenths(553, &tenths) && tenths >= 230 && tenths <= 250 && !pack_tenths(1200, &tenths);
}

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
