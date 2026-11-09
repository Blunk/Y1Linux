/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 PID1 session (docs/planning/Y1-SYSTEM-PLAN.md): mounts devtmpfs and sysfs, sets every disk
 * read-only, finds the AUXADC and the USB controller, builds the USB gadget, prepares the Y2 root and hands
 * over to the Y2 system. If the hand-over fails it stays here: battery telemetry on screen every 10 s, key
 * events on screen, and a small command shell on the USB serial gadget (/dev/ttyGS0).
 */
enum { SYS_GETDENTS64 = 217 };
enum { O_RDWR = 2, O_NOCTTY = 0400, EIO = 5, EPIPE = 32 };
enum { SCREEN = 1, USB = 2, TICKS_PER_SECOND = 10, TELEMETRY_SECONDS = 10, LOW_BATTERY_MV = 3500 };
enum { OUTPUT_LIMIT = 0x100000, WRITE_RETRIES = 100 };

static long usb_tty = -1;
static char text[512];
static unsigned text_used;
static char iio[48];      /* /sys/bus/iio/devices/iio:deviceN of the MT6323 AUXADC, empty if absent */
static char udc[96];      /* /sys/class/udc/<controller>/state, empty if absent */
static void put_pack_temperature(void);
static int charging_command(const char *line, const char *argument, long watchdog);
static void charging_setup(void);
static void put_charge_state(void);
static int registers_command(const char *line, const char *argument, long watchdog);
static int bars_command(const char *line, const char *argument, long watchdog);
static void show_bars(long watchdog);
static int backlight_command(const char *line, const char *argument, long watchdog);
static int install_y1_overrides(void);
static void switch_to_y2(void);
static int root_ready; /* tentative; defined in y1-root.h */
static void prepare_root(void);
static void usb_gadget_setup(void);
static void charge_gate(void);
static void console_to(const char *path);
static void quiet_wait_disk(long watchdog);
static int root_shell_running(void);
static int root_command(const char *line, const char *argument, long watchdog);

static void sleep_ms(unsigned ms)
{
    struct timespec64 request = { 0, (s64)ms * 1000000 };
    while (call5(SYS_CLOCK_NANOSLEEP_TIME64, CLOCK_MONOTONIC, 0, (long)&request, (long)&request, 0) == -EINTR)
        ;
}

/* Non-blocking: a PC that holds the port open without reading must not stall PID1 past the watchdog. */
static void usb_write(const char *p, unsigned n)
{
    for (unsigned tries = 0; usb_tty >= 0 && n && tries < WRITE_RETRIES;) {
        long r = call5(SYS_WRITE, usb_tty, (long)p, n, 0, 0);
        if (r == -EAGAIN || r == -EINTR) { ++tries; sleep_ms(10); continue; }
        if (r <= 0) return;
        p += r; n -= (unsigned)r;
    }
}

static void put(const char *s)
{
    while (*s && text_used < sizeof(text)) text[text_used++] = *s++;
}

static void put_number(int value)
{
    char digits[12];
    unsigned i = sizeof(digits), magnitude = value < 0 ? 0u - (unsigned)value : (unsigned)value;
    do {
        digits[--i] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (value < 0) digits[--i] = '-';
    while (i < sizeof(digits) && text_used < sizeof(text)) text[text_used++] = digits[i++];
}

static void send(unsigned where)
{
    if (where & SCREEN) write_all(text, text_used);
    if (where & USB) usb_write(text, text_used);
    text_used = 0;
}

/* out = a b c (c may be null), truncated to size. */
static void copy(char *out, unsigned size, const char *a, const char *b, const char *c)
{
    const char *parts[3] = { a, b, c };
    unsigned n = 0;
    for (unsigned i = 0; i < 3; ++i)
        for (const char *s = parts[i]; s && *s && n + 1 < size; ++s) out[n++] = *s;
    out[n] = 0;
}

/* The file's text without its trailing newline; length, or a negative error. */
static long read_text(const char *path, char *out, unsigned size)
{
    long fd = call5(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0);
    long n = fd < 0 ? fd : call5(SYS_READ, fd, (long)out, size - 1, 0, 0);
    if (fd >= 0) call5(SYS_CLOSE, fd, 0, 0, 0, 0);
    if (n < 0) { out[0] = 0; return n; }
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ')) --n;
    out[n] = 0;
    return n;
}

static int parse_number(const char *s, int *value)
{
    int sign = 1, result = 0;
    if (*s == '-') { sign = -1; ++s; }
    if (*s < '0' || *s > '9') return 0;
    while (*s >= '0' && *s <= '9') result = result * 10 + (*s++ - '0');
    *value = sign * result;
    return 1;
}

/* ---- telemetry ----------------------------------------------------------------------------------- */

/* Nominal millivolts at the pin from the 15-bit conversion: full scale 1.8 V, BATSNS/ISENSE behind the
 * PMIC's internal 4:1 divider (7.2 V). */
static unsigned millivolts(unsigned raw, unsigned full_scale_mv)
{
    return raw * full_scale_mv >> 15;
}

/* The stock board divider on VCDT: 330 kOhm over 39 kOhm (x369/39). */
static unsigned charger_millivolts(unsigned pin_mv)
{
    return pin_mv * 369 / 39;
}

static int adc_raw(const char *file, int *raw)
{
    char path[96], value[16];
    if (!iio[0]) return 0;
    copy(path, sizeof(path), iio, "/", file);
    return read_text(path, value, sizeof(value)) > 0 && parse_number(value, raw);
}

static void put_channel(const char *label, const char *file, unsigned full_scale_mv)
{
    int raw;
    put(label);
    if (adc_raw(file, &raw)) put_number((int)millivolts((unsigned)raw, full_scale_mv));
    else put("?");
}

static void put_telemetry(void)
{
    int raw, die;
    put_channel("battery ", "in_voltage7_raw", 7200);
    put_channel(" mV  isense ", "in_voltage6_raw", 7200);
    put(" mV  charger ");
    if (adc_raw("in_voltage4_raw", &raw)) put_number((int)charger_millivolts(millivolts((unsigned)raw, 1800)));
    else put("?");
    put_channel(" mV  baton ", "in_voltage5_raw", 1800);
    put(" mV  die ");
    if (adc_raw("in_temp3_input", &die)) { put_number(die / 1000); put(" C"); }
    else put("?");
    put_pack_temperature();
    put_charge_state();
    if (udc[0]) {
        char state[32];
        put("  usb ");
        put(read_text(udc, state, sizeof(state)) > 0 ? state : "?");
    }
    put("\n");
}

static void show_telemetry(void)
{
    int raw;
    put("y1-init: ");
    put_telemetry();
    if (adc_raw("in_voltage7_raw", &raw) && millivolts((unsigned)raw, 7200) < LOW_BATTERY_MV)
        put("BATTERY LOW: connect a charger\n");
    send(SCREEN);
}

/* ---- discovery ----------------------------------------------------------------------------------- */

struct dirent64 {
    u64 ino;
    s64 off;
    unsigned short reclen;
    unsigned char type;
    char name[];
};

/* Calls visit for each entry of a directory but . and ..; stops when visit returns non-zero. */
static void each_entry(const char *dir, int (*visit)(const char *dir, const struct dirent64 *entry))
{
    long fd = call5(SYS_OPEN, (long)dir, O_RDONLY, 0, 0, 0);
    if (fd < 0) return;
    for (long n; (n = call5(SYS_GETDENTS64, fd, (long)chunk, 4096, 0, 0)) > 0;) {
        for (long at = 0; at < n;) {
            const struct dirent64 *entry = (const struct dirent64 *)(chunk + at);
            at += entry->reclen;
            if (entry->name[0] == '.' && (!entry->name[1] || (entry->name[1] == '.' && !entry->name[2]))) continue;
            if (visit(dir, entry)) { call5(SYS_CLOSE, fd, 0, 0, 0, 0); return; }
        }
    }
    call5(SYS_CLOSE, fd, 0, 0, 0, 0);
}

static int find_adc(const char *dir, const struct dirent64 *entry)
{
    char device[64], path[96], name[32];
    copy(device, sizeof(device), dir, "/", entry->name);
    copy(path, sizeof(path), device, "/name", 0);
    if (read_text(path, name, sizeof(name)) <= 0 || !same(name, "mt6323-auxadc")) return 0;
    copy(iio, sizeof(iio), dir, "/", entry->name);
    return 1;
}

static int find_udc(const char *dir, const struct dirent64 *entry)
{
    char device[64];
    copy(device, sizeof(device), dir, "/", entry->name);
    copy(udc, sizeof(udc), device, "/state", 0);
    return 1;
}

/* ---- read-only protection ----------------------------------------------------------------------- */

/* BLKROSET on every block device in /proc/partitions; the flag holds until reboot. */
static void protect_disks(void)
{
    long fd = call5(SYS_OPEN, (long)"/proc/partitions", O_RDONLY, 0, 0, 0);
    long n = fd < 0 ? fd : call5(SYS_READ, fd, (long)chunk, CHUNK - 1, 0, 0);
    unsigned protected_count = 0, refused = 0;
    if (fd >= 0) call5(SYS_CLOSE, fd, 0, 0, 0, 0);
    if (n <= 0) { say("disks: /proc/partitions unreadable\n"); return; }
    chunk[n] = 0;
    for (char *line = (char *)chunk, *end; *line; line = end) {
        char path[48], *name = line;
        for (end = line; *end && *end != '\n'; ++end)
            if (*end == ' ') name = end + 1;
        if (*end) *end++ = 0;
        if (name[0] != 'm' && name[0] != 's') continue; /* the header and blank lines */
        copy(path, sizeof(path), "/dev/", name, 0);
        long disk = open_read_only(path);
        if (disk >= 0) { ++protected_count; call5(SYS_CLOSE, disk, 0, 0, 0, 0); }
        else ++refused;
    }
    put("disks: "); put_number((int)protected_count); put(" set read-only");
    if (refused) { put(", "); put_number((int)refused); put(" NOT protected"); }
    put("\n");
    send(SCREEN);
}

/* ---- the USB shell ------------------------------------------------------------------------------- */

static void usb_say(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    usb_write(s, n);
}

static void shell_help(void)
{
    usb_say("commands (none writes anything):\n"
              "  bat          battery, charger and USB state\n"
              "  dmesg        the kernel log\n"
              "  cat PATH     a file, up to 1 MiB (/proc, /sys, ...)\n"
              "  ls PATH      a directory\n"
              "  disks        /proc/partitions and the read-only flags\n"
              "  pmic         MT6323 charger registers, read through regmap debugfs\n"
              "  regs         display registers (read-only /dev/mem) and the PMIC ISINK block\n"
              "  bars         colour bars on the DRM frame buffer for 10 s\n"
              "  bl [N]       backlight brightness 0..32 (the only command that writes)\n"
              "  rootsh       the Y2 root filesystem's shell (exit returns here)\n"
              "  reboot       restart the player\n");
}

static void shell_cat(const char *path, long watchdog)
{
    long fd = call5(SYS_OPEN, (long)path, O_RDONLY, 0, 0, 0);
    unsigned total = 0;
    if (fd < 0) { put("cat: cannot open "); put(path); put("\n"); send(USB); return; }
    for (long n; total < OUTPUT_LIMIT && (n = call5(SYS_READ, fd, (long)chunk, 2048, 0, 0)) > 0; total += (unsigned)n) {
        usb_write((const char *)chunk, (unsigned)n);
        feed(watchdog);
    }
    call5(SYS_CLOSE, fd, 0, 0, 0, 0);
}

static int list_entry(const char *dir, const struct dirent64 *entry)
{
    (void)dir;
    put(entry->name);
    put(entry->type == 4 ? "/\n" : entry->type == 10 ? "@\n" : "\n");
    send(USB);
    return 0;
}

/* Each /dev/kmsg read returns one record "level,sequence,usec,flags;text\n"; only the text is shown. */
static void shell_dmesg(long watchdog)
{
    char record[1024];
    long fd = call5(SYS_OPEN, (long)"/dev/kmsg", O_RDONLY | O_NONBLOCK, 0, 0, 0);
    if (fd < 0) { usb_say("dmesg: /dev/kmsg unavailable\n"); return; }
    for (unsigned count = 0;; ++count) {
        long n = call5(SYS_READ, fd, (long)record, sizeof(record), 0, 0);
        if (n == -EPIPE || n == -EINTR) continue;
        if (n <= 0) break;
        long start = 0;
        while (start < n && record[start] != ';') ++start;
        if (start < n) usb_write(record + start + 1, (unsigned)(n - start - 1));
        if (count % 64 == 0) feed(watchdog);
    }
    call5(SYS_CLOSE, fd, 0, 0, 0, 0);
}

static int show_read_only_flag(const char *dir, const struct dirent64 *entry)
{
    char disk[64], path[96], flag[8];
    copy(disk, sizeof(disk), dir, "/", entry->name);
    copy(path, sizeof(path), disk, "/ro", 0);
    put(entry->name); put(" ro=");
    put(read_text(path, flag, sizeof(flag)) > 0 ? flag : "?");
    put("\n");
    send(USB);
    return 0;
}

static int contains(const char *haystack, long n, const char *word)
{
    for (long i = 0; i < n; ++i) {
        long j = 0;
        while (word[j] && i + j < n && haystack[i + j] == word[j]) ++j;
        if (!word[j]) return 1;
    }
    return 0;
}

static void shell_reboot(void)
{
    usb_say("rebooting\n");
    say("y1-init: reboot requested over USB\n");
    sleep_seconds(1);
    call5(SYS_REBOOT, (long)0xfee1dead, 0x28121969, 0x01234567, 0, 0);
}

static void shell_line(char *line, long watchdog)
{
    char *argument = line;
    while (*argument && *argument != ' ') ++argument;
    if (*argument) *argument++ = 0;
    while (*argument == ' ') ++argument;
    put("y1-init: usb> "); put(line); put(" "); put(argument); put("\n"); send(SCREEN);
    if (!line[0]) { usb_say("y1# "); return; }
    if (same(line, "help")) shell_help();
    else if (same(line, "bat")) { put_telemetry(); send(USB); }
    else if (same(line, "dmesg")) shell_dmesg(watchdog);
    else if (same(line, "cat") && *argument) shell_cat(argument, watchdog);
    else if (same(line, "ls") && *argument) each_entry(argument, list_entry);
    else if (same(line, "disks")) { shell_cat("/proc/partitions", watchdog); each_entry("/sys/block", show_read_only_flag); }
    else if (same(line, "reboot")) shell_reboot();
    else if (charging_command(line, argument, watchdog)) {}
    else if (registers_command(line, argument, watchdog)) {}
    else if (bars_command(line, argument, watchdog)) {}
    else if (backlight_command(line, argument, watchdog)) {}
    else if (root_command(line, argument, watchdog)) { return; }
    else usb_say("unknown command; type help\n");
    usb_say("y1# ");
}

/* The tty is in canonical mode (the gadget's default): each read is one line, echoed by the tty. */
/* Opened once a second until it works; a hang-up (the PC closed the port or the cable went) reopens. */
static void poll_shell(unsigned tick, long watchdog)
{
    static int announced;
    char line[256];
    if (root_shell_running()) return;
    if (usb_tty < 0) {
        if (tick % TICKS_PER_SECOND) return;
        usb_tty = call5(SYS_OPEN, (long)"/dev/ttyGS0", O_RDWR | O_NOCTTY | O_NONBLOCK, 0, 0, 0);
        if (usb_tty < 0) return;
        if (!announced) say("y1-init: USB serial shell on /dev/ttyGS0\n");
        announced = 1;
        usb_say("\nY1 shell; type help\ny1# ");
    }
    long n = call5(SYS_READ, usb_tty, (long)line, sizeof(line) - 1, 0, 0);
    if (n == 0 || n == -EIO) { call5(SYS_CLOSE, usb_tty, 0, 0, 0, 0); usb_tty = -1; return; }
    if (n < 0) return;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) --n;
    line[n] = 0;
    shell_line(line, watchdog);
}

/* ---- session ------------------------------------------------------------------------------------- */

static int telemetry_selftest(void)
{
    return millivolts(16384, 7200) == 3600 && millivolts(32767, 1800) == 1799 &&
           charger_millivolts(507) == 4797;
}

static void run_session(long watchdog)
{
    char path[] = "/dev/input/event0", last_state[32], state[32];
    long keys[INPUT_NODES];
    last_state[0] = 0;
    if (call5(SYS_MOUNT, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", MS_SILENT, 0) != 0)
        say("devtmpfs: mount failed\n");
    if (call5(SYS_MOUNT, (long)"sysfs", (long)"/sys", (long)"sysfs", MS_SILENT, 0) != 0)
        say("sysfs: mount failed\n");
    charging_setup();
    quiet_wait_disk(watchdog);
    protect_disks();
    each_entry("/sys/bus/iio/devices", find_adc);
    each_entry("/sys/class/udc", find_udc);
    usb_gadget_setup();
    say(iio[0] ? "adc: " : "adc: MT6323 AUXADC not found\n");
    if (iio[0]) { say(iio); say("\n"); }
    say(udc[0] ? "usb: " : "usb: no device controller\n");
    if (udc[0]) { say(udc); say("\n"); }
    prepare_root();
    if (root_ready) charge_gate();
    if (root_ready) switch_to_y2();
    /* The Y2 system did not start: the screen gets PID1's text again. */
    console_to("/dev/tty0");
    say("Y1: the system did not start; PID1's log is on the serial console, its shell on USB\n");
    for (unsigned i = 0; i < INPUT_NODES; ++i) {
        path[16] = (char)('0' + i);
        keys[i] = call5(SYS_OPEN, (long)path, O_RDONLY | O_NONBLOCK, 0, 0, 0);
    }
    say("\ny1-init: session running; connect USB and open the serial port\n");
    for (unsigned tick = 0;; ++tick) {
        if (tick % TICKS_PER_SECOND == 0) feed(watchdog);
        if (tick % (TICKS_PER_SECOND * TELEMETRY_SECONDS) == 0) show_telemetry();
        if (tick % TICKS_PER_SECOND == 0 && udc[0] && read_text(udc, state, sizeof(state)) > 0 &&
            !same(state, last_state)) {
            put("y1-init: usb "); put(state); put("\n"); send(SCREEN);
            copy(last_state, sizeof(last_state), state, 0, 0);
        }
        for (unsigned i = 0; i < INPUT_NODES; ++i) {
            struct input_event32 e;
            while (keys[i] >= 0 && call5(SYS_READ, keys[i], (long)&e, sizeof(e), 0, 0) == (long)sizeof(e))
                show_event(&e);
        }
        poll_shell(tick, watchdog);
        sleep_tenths(1);
    }
}
