/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 PID1 updates (docs/planning/Y1-RECOVERY-PLAN.md, R4), the Y2's initramfs/production/recovery.sh
 * and its init's update rescue in PID1's C. Both images carry the installer in the initramfs
 * (tools/build/y1_installer.py): y1-update-core and y2-recovery-ui.
 *
 * Y2DATA is mounted only when its superblock is the Y1's Y2DATA (ext4, label, UUID) and records no error; the
 * mount replays the journal, which is what e2fsck -p does after an unclean stop. e2fsck itself does not fit both
 * 6 MiB images; the normal system's S03y1-data runs it at every start.
 *
 * Recovery (y2.recovery=1, RECOVERY's tree): attach Y2DATA, check and mount it at /newdata, run
 * `y1-update-core recovery` with the recovery screen, keep the evidence on Y2DATA, then restart into the result
 * or, with the owner's /data/updates/restart.json {"mode": "poweroff"}, turn off. The watchdog is fed only while
 * the installer's progress moves (a frozen kernel or two minutes without progress restart the player; the
 * request and the RTC flag stay set, so the next start resumes). Never starts the normal system, never formats.
 * An unknown outcome keeps PID1's session and USB shell; after ten minutes without a charger the player turns off.
 *
 * Rescue (BOOTIMG, before the root is mounted, so a torn Y2ROOT does not stop it): with an update journal on
 * Y2DATA, `y1-update-core boot-check` counts the first starts of a new system or requests the recovery again
 * (flag and MISC, three times at most) and restarts into it.
 */
enum { SYS_SYNC = 36, SYS_KILL = 37, SYS_RENAME = 38, SYS_UMOUNT2 = 52 };
enum { SIGKILL = 9, SIGTERM = 15, STALL_INSTALL = 120, STALL_PREPARE = 300, CHARGER_WAIT = 600,
       STUCK_ON_BATTERY = 600 };

static const char *const program_env[] = { "PATH=/sbin:/bin", "HOME=/", 0 };
static int data_mounted, restart_after, recovery_stuck_on;
static unsigned stuck_idle;
static long recovery_watchdog = -1, recovery_ui = -1;

/* ---- small helpers -------------------------------------------------------------------------------------- */

static int write_file(const char *path, const char *data, unsigned n)
{
    long fd = call5(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0600, 0, 0);
    long w = fd < 0 ? fd : call5(SYS_WRITE, fd, (long)data, n, 0, 0);
    if (fd >= 0) call5(SYS_CLOSE, fd, 0, 0, 0, 0);
    return w == (long)n;
}

/* A small file (< CHUNK) copied whole; 1 on success. */
static int copy_file(const char *from, const char *to)
{
    long in = call5(SYS_OPEN, (long)from, O_RDONLY, 0, 0, 0);
    long n = in < 0 ? in : call5(SYS_READ, in, (long)chunk, CHUNK, 0, 0);
    if (in >= 0) call5(SYS_CLOSE, in, 0, 0, 0, 0);
    return n >= 0 && n < CHUNK && write_file(to, (const char *)chunk, (unsigned)n);
}

/* The value of "key":"value" in the core's plain JSON (lower-case letters and _), empty if absent. */
static void json_word(const char *json, const char *key, char *out, unsigned size)
{
    unsigned n = 0;
    for (const char *p = json; *p && !n; ++p) {
        const char *a = p, *b = key;
        if (*a++ != '"') continue;
        while (*b && *a == *b) { ++a; ++b; }
        if (*b || a[0] != '"' || a[1] != ':' || a[2] != '"') continue;
        for (a += 3; ((*a >= 'a' && *a <= 'z') || *a == '_') && n + 1 < size; ++a) out[n++] = *a;
    }
    out[n] = 0;
}

/* restart.json asks to turn off instead of restarting: {"mode": "poweroff"}. */
static int wants_poweroff(const char *json)
{
    for (const char *p = json; *p; ++p) {
        const char *a = p, *b = "\"mode\"";
        while (*b && *a == *b) { ++a; ++b; }
        if (*b) continue;
        while (*a == ' ' || *a == ':') ++a;
        b = "\"poweroff\"";
        while (*b && *a == *b) { ++a; ++b; }
        return !*b;
    }
    return 0;
}

static int charger_found;
static int find_charger(const char *dir, const struct dirent64 *entry)
{
    char supply[64], path[96], value[16];
    copy(supply, sizeof(supply), dir, "/", entry->name);
    copy(path, sizeof(path), supply, "/type", 0);
    if (read_text(path, value, sizeof(value)) <= 0 || (!same(value, "USB") && !same(value, "Mains"))) return 0;
    copy(path, sizeof(path), supply, "/online", 0);
    charger_found = read_text(path, value, sizeof(value)) > 0 && same(value, "1");
    return charger_found;
}

/* A USB or mains supply online, as y1-update-core's battery check reads it. */
static int charger_online(void)
{
    charger_found = 0;
    each_entry("/sys/class/power_supply", find_charger);
    return charger_found;
}

/* Runs a program of the initramfs with its output in out (or PID1's console); its pid, or a negative error. */
static long spawn(const char *const argv[], const char *out)
{
    long pid = call5(SYS_FORK, 0, 0, 0, 0, 0);
    if (pid != 0) return pid;
    if (out) {
        long fd = call5(SYS_OPEN, (long)out, O_WRONLY | O_CREAT | O_TRUNC, 0600, 0, 0);
        if (fd < 0) call5(SYS_EXIT, 126, 0, 0, 0, 0);
        call5(SYS_DUP2, fd, 1, 0, 0, 0);
    }
    for (long fd = 3; fd < 64; ++fd) call5(SYS_CLOSE, fd, 0, 0, 0, 0); /* never the watchdog */
    call5(SYS_EXECVE, (long)argv[0], (long)argv, (long)program_env, 0, 0);
    call5(SYS_EXIT, 127, 0, 0, 0, 0);
    return -1;
}

/* Waits for pid; with a watchdog, feeds it once a second while progress (a file) changed within stall seconds.
 * The exit code, or -1 for a signal or a failed start. */
static int wait_watched(long pid, const char *progress, unsigned stall)
{
    char last[64], now[64]; /* no initialiser: there is no memset */
    unsigned still = 0;
    int status;
    last[0] = 0;
    if (pid < 0) return -1;
    for (;;) {
        long r = call5(SYS_WAIT4, pid, (long)&status, WNOHANG, 0, 0);
        if (r == pid) return status & 0x7f ? -1 : (status >> 8) & 0xff;
        if (r < 0) return -1;
        if (progress && read_text(progress, now, sizeof(now)) >= 0 && !same(now, last)) {
            copy(last, sizeof(last), now, 0, 0);
            still = 0;
        } else if (++still == stall && recovery_watchdog >= 0) {
            put("y1-update: no progress for "); put_number((int)stall); put(" s ("); put(last); put("); watchdog restart\n");
            send(SCREEN);
        }
        if (still < stall) feed(recovery_watchdog);
        sleep_seconds(1);
    }
}

static int run(const char *const argv[], const char *out)
{
    return wait_watched(spawn(argv, out), 0, STALL_PREPARE);
}

static void log_kernel(const char *a, const char *b)
{
    char line[160];
    copy(line, sizeof(line), "<6>Y1UPDATE: ", a, b);
    long fd = call5(SYS_OPEN, (long)"/dev/kmsg", O_WRONLY, 0, 0, 0);
    unsigned n = 0;
    while (line[n]) ++n;
    if (fd >= 0) { call5(SYS_WRITE, fd, (long)line, n, 0, 0); call5(SYS_CLOSE, fd, 0, 0, 0, 0); }
    say("y1-update: "); say(a); say(b ? b : ""); say("\n");
}

/* The ext4 superblock (1024 bytes from byte 1024): magic 0xef53 at 0x38, s_state at 0x3a (bit 1: errors found),
 * s_uuid at 0x68, s_volume_name at 0x78. The Y1's Y2DATA: label Y2DATA, UUID 79314c69-6e75-4801-8000-000000000102
 * (initramfs/y1-data/etc/init.d/S03y1-data). */
static int y2data_superblock(const u8 *sb)
{
    static const u8 uuid[16] = { 0x79, 0x31, 0x4c, 0x69, 0x6e, 0x75, 0x48, 0x01,
                                 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02 };
    static const char label[16] = "Y2DATA";
    int match = sb[0x38] == 0x53 && sb[0x39] == 0xef && !(sb[0x3a] & 2);
    for (unsigned i = 0; match && i < 16; ++i) match = sb[0x68 + i] == uuid[i] && sb[0x78 + i] == (u8)label[i];
    return match;
}

static int data_superblock_ok(void)
{
    u8 *sb = chunk;
    long fd = call5(SYS_OPEN, (long)data_device, O_RDONLY, 0, 0, 0);
    long n = fd < 0 ? fd : call5(SYS_READ, fd, (long)sb, 2048, 0, 0);
    if (fd >= 0) call5(SYS_CLOSE, fd, 0, 0, 0, 0);
    return n == 2048 && y2data_superblock(sb + 1024);
}

/* Y2DATA mounted read-write at /newdata; 1 when it is ready. */
static int mount_data(void)
{
    char schema[8];
    if (!data_superblock_ok()) { log_kernel("Y2DATA superblock: not the Y1's Y2DATA, or errors recorded", 0); return 0; }
    call5(SYS_MKDIR, (long)"/newdata", 0755, 0, 0, 0);
    if (!mount_step(data_device, "/newdata", "ext4", MS_NOSUID | MS_NODEV, 0)) return 0;
    data_mounted = 1;
    if (read_text("/newdata/.y2data-schema", schema, sizeof(schema)) > 0 && same(schema, "1")) return 1;
    log_kernel("Y2DATA schema is not 1", 0);
    return 0;
}

static void unmount_data(void)
{
    call5(SYS_SYNC, 0, 0, 0, 0, 0);
    if (data_mounted && call5(SYS_UMOUNT2, (long)"/newdata", 0, 0, 0, 0) == 0) data_mounted = 0;
}

/* /run/y2 on a tmpfs, for the core's progress and the screen's text. */
static void runtime_dir(void)
{
    call5(SYS_MKDIR, (long)"/run", 0755, 0, 0, 0);
    call5(SYS_MOUNT, (long)"tmpfs", (long)"/run", (long)"tmpfs", MS_NOSUID | MS_NODEV, (long)"size=4m,mode=0755");
    call5(SYS_MKDIR, (long)"/run/y2", 0755, 0, 0, 0);
}

/* ---- rescue (BOOTIMG) ----------------------------------------------------------------------------------- */

/* 1 when the system may start. Without Y2DATA or a journal there is nothing to decide here (S03y1-data
 * reports a missing Y2DATA). */
static int update_rescue(void)
{
    static const char *const needed[] = { "/sbin/y1-update-core", "boot-needed", 0 };
    static const char *const check[] = { "/sbin/y1-update-core", "boot-check", 0 };
    if (!data_device[0]) return 1;
    call5(SYS_MKDIR, (long)"/newdata", 0755, 0, 0, 0);
    if (call5(SYS_MOUNT, (long)data_device, (long)"/newdata", (long)"ext4", MS_RDONLY, (long)"noload") != 0)
        return 1;
    long journal = call5(SYS_OPEN, (long)"/newdata/updates/state.json", O_RDONLY, 0, 0, 0);
    int decision = journal < 0 ? 10 : run(needed, 0);
    if (journal >= 0) call5(SYS_CLOSE, journal, 0, 0, 0, 0);
    call5(SYS_UMOUNT2, (long)"/newdata", 0, 0, 0, 0);
    if (decision == 10) return 1;
    if (decision != 0) { log_kernel("rescue: update journal unreadable", 0); return 0; }
    runtime_dir();
    if (!mount_data()) { unmount_data(); log_kernel("rescue: Y2DATA not ready for the update check", 0); return 0; }
    decision = run(check, "/run/y2/update-boot-check.json");
    copy_file("/run/y2/update-boot-check.json", "/newdata/updates/boot-check-last.json");
    unmount_data();
    call5(SYS_UMOUNT2, (long)"/run", 0, 0, 0, 0);
    if (decision == 10) return 1;
    if (decision == 20) {
        /* The core set the request and the RTC flag: LK starts RECOVERY at this restart. */
        log_kernel("rescue: update unfinished; recovery requested; restart", 0);
        sleep_seconds(1);
        call5(SYS_REBOOT, (long)0xfee1dead, 0x28121969, 0x01234567, 0, 0);
    }
    log_kernel("rescue: update needs attention; see /data/updates/boot-check-last.json", 0);
    return 0;
}

/* ---- recovery (RECOVERY) -------------------------------------------------------------------------------- */

/* Title, then body lines (null-terminated list), for y2-recovery-ui. */
static void screen(const char *const lines[])
{
    char text[256];
    unsigned n = 0;
    for (unsigned i = 0; lines[i]; ++i) {
        for (const char *s = lines[i]; *s && n + 1 < sizeof(text); ++s) text[n++] = *s;
        if (n + 1 < sizeof(text)) text[n++] = '\n';
    }
    if (write_file("/run/y2/recovery-screen.tmp", text, n))
        call5(SYS_RENAME, (long)"/run/y2/recovery-screen.tmp", (long)"/run/y2/recovery-screen", 0, 0, 0);
    say("y1-recovery: "); say(lines[0]); say("\n");
}

static void watchdog_release(void)
{
    if (recovery_watchdog < 0) return;
    call5(SYS_WRITE, recovery_watchdog, (long)"V", 1, 0, 0); /* magic close: back to the kernel */
    call5(SYS_CLOSE, recovery_watchdog, 0, 0, 0, 0);
    recovery_watchdog = -1;
}

/* The result and the kernel log on Y2DATA, where the normal system's diagnostics read them. */
static void recovery_evidence(void)
{
    char record[1024];
    if (!data_mounted) return;
    call5(SYS_MKDIR, (long)"/newdata/logs", 0755, 0, 0, 0);
    copy_file("/run/y2/recovery-result.json", "/newdata/updates/recovery-last.json");
    long log = call5(SYS_OPEN, (long)"/newdata/logs/recovery-last.log", O_WRONLY | O_CREAT | O_TRUNC, 0600, 0, 0);
    long kmsg = call5(SYS_OPEN, (long)"/dev/kmsg", O_RDONLY | O_NONBLOCK, 0, 0, 0);
    for (long n; log >= 0 && kmsg >= 0 && (n = call5(SYS_READ, kmsg, (long)record, sizeof(record), 0, 0)) != -EAGAIN;) {
        if (n == -EPIPE || n == -EINTR) continue;
        if (n <= 0) break;
        long start = 0;
        while (start < n && record[start] != ';') ++start;
        if (start < n) call5(SYS_WRITE, log, (long)(record + start + 1), n - start - 1, 0, 0);
    }
    if (kmsg >= 0) call5(SYS_CLOSE, kmsg, 0, 0, 0, 0);
    if (log >= 0) call5(SYS_CLOSE, log, 0, 0, 0, 0);
    call5(SYS_SYNC, 0, 0, 0, 0, 0);
}

/* A still, dark screen for LK (the Y2's F23): the light off, the screen's process gone. */
static void recovery_display_off(void)
{
    int status;
    set_brightness("0");
    if (recovery_ui < 0) return;
    call5(SYS_KILL, recovery_ui, SIGTERM, 0, 0, 0);
    for (unsigned i = 0; i < 20 && call5(SYS_WAIT4, recovery_ui, (long)&status, WNOHANG, 0, 0) == 0; ++i) sleep_ms(50);
    call5(SYS_KILL, recovery_ui, SIGKILL, 0, 0, 0);
    call5(SYS_WAIT4, recovery_ui, (long)&status, 0, 0, 0);
    recovery_ui = -1;
    set_brightness("0");
}

/* Keep the evidence, then restart into the result or turn off. Never returns. */
static void recovery_off(unsigned seconds)
{
    recovery_evidence();
    unmount_data();
    sleep_seconds(seconds);
    recovery_display_off();
    watchdog_release();
    log_kernel(restart_after ? "restart" : "power off", 0);
    sleep_seconds(1);
    call5(SYS_REBOOT, (long)0xfee1dead, 0x28121969, restart_after ? 0x01234567 : 0x4321fedc, 0, 0);
    for (;;) sleep_seconds(60);
}

/* An unknown state: the request stays (LK returns here at the next start with the flag), a person decides with
 * PID1's USB shell. The session calls recovery_idle() every second. */
static void recovery_stuck(const char *why)
{
    const char *const lines[] = { "Update needs attention", why, "", "Connect the player", "to a computer.", 0 };
    watchdog_release();
    log_kernel("staying in recovery: ", why);
    screen(lines);
    recovery_evidence();
    recovery_stuck_on = 1;
}

static void recovery_idle(void)
{
    static const char *const lines[] = { "Update needs attention", "", "Turning off to save power.",
                                         "Press Power to see it again.", 0 };
    if (!recovery_stuck_on) return;
    stuck_idle = charger_online() ? 0 : stuck_idle + 1;
    if (stuck_idle < STUCK_ON_BATTERY) return;
    log_kernel("stuck on battery for ten minutes; power off", 0);
    screen(lines);
    restart_after = 0;
    recovery_off(8);
}

/* The screen and the timing for each final outcome, then off. */
static void recovery_finish(const char *outcome)
{
    static const char *const installed[] = { "Update installed", "", "Starting the new version.", 0 };
    static const char *const installed_off[] = { "Update installed", "", "The player turns off now.",
                                                 "Press Power to start.", 0 };
    static const char *const rolled_back[] = { "Update not installed", "The previous version", "was restored.", "",
                                               "Restarting.", 0 };
    static const char *const rolled_back_off[] = { "Update not installed", "The previous version", "was restored.",
                                                   "", "Press Power to start.", 0 };
    static const char *const unchanged[] = { "Update not installed", "Nothing was changed.", "", "Restarting.", 0 };
    static const char *const unchanged_off[] = { "Update not installed", "Nothing was changed.", "",
                                                 "The player turns off now.", "Press Power to start.", 0 };
    static const char *const probe[] = { "Recovery works", "", "Restarting.", 0 };
    static const char *const probe_off[] = { "Recovery works", "", "The player turns off now.",
                                             "Press Power to start.", 0 };
    static const char *const nothing[] = { "Nothing to install", "", "Restarting.", 0 };
    static const char *const nothing_off[] = { "Nothing to install", "", "The player turns off now.",
                                               "Press Power to start.", 0 };
    if (same(outcome, "installed")) { screen(restart_after ? installed : installed_off); recovery_off(restart_after ? 1 : 12); }
    if (same(outcome, "rolled_back")) { screen(restart_after ? rolled_back : rolled_back_off); recovery_off(restart_after ? 8 : 12); }
    if (same(outcome, "failed_unchanged")) { screen(restart_after ? unchanged : unchanged_off); recovery_off(restart_after ? 8 : 12); }
    if (same(outcome, "probe")) { screen(restart_after ? probe : probe_off); recovery_off(restart_after ? 4 : 12); }
    if (same(outcome, "nothing")) { screen(restart_after ? nothing : nothing_off); recovery_off(restart_after ? 4 : 6); }
    recovery_stuck("The update stopped");
}

/* Returns only when the recovery needs a person (recovery_stuck); otherwise restarts or turns off. */
static void recovery_run(void)
{
    static const char *const installing[] = { "Installing Update", "Don't turn off the player.",
                                              "This takes under a minute.", 0 };
    static const char *const charge[] = { "Connect the charger", "The battery is too low", "to install the update.", 0 };
    static const char *const too_low[] = { "Battery too low", "Charge the player, then", "press Power to continue.", 0 };
    static const char *const ui[] = { "/sbin/y2-recovery-ui", 0 };
    static const char *const install[] = { "/sbin/y1-update-core", "recovery", 0 };
    char result[1024], outcome[24], restart[256];
    runtime_dir();
    recovery_watchdog = call5(SYS_OPEN, (long)"/dev/watchdog", O_WRONLY, 0, 0, 0);
    screen(installing);
    recovery_ui = spawn(ui, 0);
    log_kernel("recovery started", 0);
    if (!data_device[0]) { recovery_stuck("Storage not found"); return; }
    if (!mount_data()) { recovery_stuck("Storage check failed"); return; }
    /* Restart into the result (the Y2's OTA-46 default) unless the owner asked to turn off. */
    restart_after = !(read_text("/newdata/updates/restart.json", restart, sizeof(restart)) > 0 && wants_poweroff(restart));
    for (;;) {
        int code = wait_watched(spawn(install, "/run/y2/recovery-result.json"), "/run/y2/recovery-progress", STALL_INSTALL);
        if (read_text("/run/y2/recovery-result.json", result, sizeof(result)) < 0) result[0] = 0;
        json_word(result, "outcome", outcome, sizeof(outcome));
        put("y1-recovery: core exit "); put_number(code); put(", outcome "); put(outcome[0] ? outcome : "none"); put("\n");
        send(SCREEN);
        log_kernel("outcome=", outcome[0] ? outcome : "none");
        if (!same(outcome, "needs_power")) break;
        /* A low battery and no charger: wait ten minutes for one, else off (the request stays set). */
        screen(charge);
        unsigned waited = 0;
        for (; !charger_online() && waited < CHARGER_WAIT; waited += 2) { feed(recovery_watchdog); sleep_seconds(2); }
        if (!charger_online()) { screen(too_low); restart_after = 0; recovery_off(10); }
        screen(installing);
    }
    recovery_finish(outcome);
}

static int update_selftest(void)
{
    char word[24];
    u8 *sb = chunk;
    static const u8 good[] = { 0x79, 0x31, 0x4c, 0x69, 0x6e, 0x75, 0x48, 0x01,
                               0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 'Y', '2', 'D', 'A', 'T', 'A' };
    for (unsigned i = 0; i < 1024; ++i) sb[i] = 0;
    sb[0x38] = 0x53; sb[0x39] = 0xef; sb[0x3a] = 1;
    for (unsigned i = 0; i < sizeof(good); ++i) sb[0x68 + i] = good[i];
    if (!y2data_superblock(sb)) return 0;
    sb[0x3a] = 3;
    if (y2data_superblock(sb)) return 0;
    sb[0x3a] = 1; sb[0x69] = 0x32; /* the Y2's Y2DATA, 79324c69-... */
    if (y2data_superblock(sb)) return 0;
    sb[0x69] = 0x31; sb[0x7e] = 'X'; /* label Y2DATAX */
    if (y2data_superblock(sb)) return 0;
    sb[0x7e] = 0; sb[0x38] = 0;
    if (y2data_superblock(sb)) return 0;
    json_word("{\"schema\":2,\"outcome\":\"rolled_back\",\"ok\":true}", "outcome", word, sizeof(word));
    if (!same(word, "rolled_back")) return 0;
    json_word("{\"outcomes\":\"x\",\"outcome\":\"probe\"}", "outcome", word, sizeof(word));
    if (!same(word, "probe")) return 0;
    json_word("{\"state\":\"Idle\"}", "outcome", word, sizeof(word));
    if (word[0]) return 0;
    json_word("{\"outcome\":\"failed_unchanged_and_a_much_longer_tail\"}", "outcome", word, 8);
    if (!same(word, "failed_")) return 0;
    return wants_poweroff("{\"mode\": \"poweroff\"}") && wants_poweroff("{\"mode\":\"poweroff\"}") &&
           !wants_poweroff("{\"mode\": \"restart\"}") && !wants_poweroff("{}") &&
           !wants_poweroff("{\"mode\": \"poweroffx\"}") && !wants_poweroff("");
}
