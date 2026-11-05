/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 eighth-boot additions (docs/planning/Y1-BACKLIGHT-PLAN.md), included after
 * y1-seventh-boot.h only when Y1_EIGHTH_BOOT is defined, so the earlier PID1 binaries stay byte-identical.
 * `bl N` is the shell's only write command: it writes N to the backlight's brightness file (MT6323
 * ISINK0/1, never above LK's duty; the kernel driver and the PMIC wrapper's policy bound the rest), then
 * shows the brightness and reads the ISINK block back. `bl` alone only reads.
 */
static const char backlight_dir[] = "/sys/class/backlight/y2-backlight";

static void show_backlight(void)
{
    char path[80], value[16];
    static const char *const files[] = { "brightness", "actual_brightness", "max_brightness" };
    for (unsigned i = 0; i < 3; ++i) {
        copy(path, sizeof(path), backlight_dir, "/", files[i]);
        put(files[i]); put("="); put(read_text(path, value, sizeof(value)) > 0 ? value : "?"); put("  ");
    }
    put("\n");
    send(USB);
}

static void shell_backlight(const char *argument)
{
    char path[80];
    int level;
    if (*argument) {
        unsigned n = 0;
        if (!parse_number(argument, &level) || level < 0 || level > 32) { usb_say("bl: 0..32\n"); return; }
        copy(path, sizeof(path), backlight_dir, "/brightness", 0);
        long fd = call5(SYS_OPEN, (long)path, O_WRONLY, 0, 0, 0);
        while (argument[n] >= '0' && argument[n] <= '9') ++n;
        long r = fd < 0 ? fd : call5(SYS_WRITE, fd, (long)argument, n, 0, 0);
        if (fd >= 0) call5(SYS_CLOSE, fd, 0, 0, 0, 0);
        put("bl: wrote "); put_number(level); put(r == (long)n ? "\n" : " FAILED\n"); send(USB);
        put("Y1 EIGHTH BOOT: backlight "); put_number(level); put(r == (long)n ? "\n" : " refused\n"); send(SCREEN);
    }
    show_backlight();
    shell_isink();
}

static int eighth_command(const char *line, const char *argument, long watchdog)
{
    (void)watchdog;
    if (!same(line, "bl")) return 0;
    shell_backlight(argument);
    return 1;
}
