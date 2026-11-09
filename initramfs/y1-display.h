/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 PID1 display and backlight shell commands (docs/planning/Y1-DISPLAY-PLAN.md,
 * Y1-BACKLIGHT-PLAN.md). `regs`: a read-only dump of the display registers, each window mapped PROT_READ from
 * /dev/mem and read with 32-bit loads; the display power domain is checked first and each block's MMSYS clock
 * gate before its window, because a read from an unpowered or unclocked block can stall the bus. `bars`: eight
 * colour bars through the MediaTek DRM driver's fbdev emulation for ten seconds, then the screen restored.
 * `bl N` is the shell's only write command: it writes N to the backlight's brightness file (MT6323 ISINK0/1,
 * never above LK's duty; the kernel driver and the PMIC wrapper's policy bound the rest), then shows the
 * brightness and reads the ISINK block back. `bl` alone only reads.
 */
enum { SYS_MMAP2 = 192, SYS_MUNMAP = 91 };
enum { O_SYNC = 04010000, PROT_READ = 1, MAP_SHARED = 1, PAGE = 4096 };
enum { SPM = 0x10006000, MMSYS = 0x14000000, NO_GATE = 0xff };

static long call6(long number, long a, long b, long c, long d, long e, long f)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r5 __asm__("r5") = f;
    register long r7 __asm__("r7") = number;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
                     : "memory", "cc");
    return r0;
}

struct window {
    const char *name;
    u32 base, length;
    unsigned char gate_group, gate_bit; /* MMSYS CG_CON0 (0x100) or CG_CON1 (0x110); NO_GATE */
};

/* Fork mt6572.dtsi bases; gate bits from clk-mt6572-mm.c (a set bit is a gated clock). */
static const struct window windows[] = {
    { "mmsys", MMSYS, 0x200, NO_GATE, 0 },
    { "smi-common", 0x1000e000, 0x100, 0, 0 },
    { "larb0", 0x14010000, 0x100, 0, 1 },
    { "ovl", 0x14007000, 0x300, 0, 8 },
    { "rdma", 0x14008000, 0x100, 0, 7 },
    { "bls", 0x1400a000, 0x100, 0, 5 },
    { "color", 0x1400b000, 0x100, 0, 4 },
    { "dsi", 0x1400c000, 0x200, 1, 0 },
    { "mutex", 0x1400e000, 0x100, 0, 0 },
    { "mipi-tx", 0x10011000, 0x100, NO_GATE, 0 },
};

static long dev_mem = -1;

/* A read-only mapping of the page holding base; the caller unmaps PAGE bytes. */
static volatile const u32 *map_page(u32 base)
{
    if (dev_mem < 0) dev_mem = call5(SYS_OPEN, (long)"/dev/mem", O_RDONLY | O_SYNC, 0, 0, 0);
    if (dev_mem < 0) return 0;
    long p = call6(SYS_MMAP2, 0, PAGE, PROT_READ, MAP_SHARED, dev_mem, (long)(base / PAGE));
    return p < 0 && p > -4096 ? 0 : (volatile const u32 *)p;
}

static int read_word(u32 address, u32 *value)
{
    volatile const u32 *page = map_page(address & ~(u32)(PAGE - 1));
    if (!page) return 0;
    *value = page[(address & (PAGE - 1)) / 4];
    call5(SYS_MUNMAP, (long)page, PAGE, 0, 0, 0);
    return 1;
}

static void dump_window(const struct window *w, long watchdog)
{
    volatile const u32 *page = map_page(w->base);
    if (!page) { put(w->name); put(": /dev/mem mapping refused\n"); send(USB); return; }
    put("== "); put(w->name); put(" 0x"); put_hex(w->base, 8); put("\n"); send(USB);
    for (u32 offset = 0; offset < w->length; offset += 16) {
        u32 row[4];
        for (unsigned i = 0; i < 4; ++i) row[i] = page[(offset / 4) + i];
        if (!(row[0] | row[1] | row[2] | row[3])) continue;
        put_hex(offset, 3); put(":");
        for (unsigned i = 0; i < 4; ++i) { put(" "); put_hex(row[i], 8); }
        put("\n"); send(USB);
    }
    call5(SYS_MUNMAP, (long)page, PAGE, 0, 0, 0);
    feed(watchdog);
}

static void shell_regs(long watchdog)
{
    u32 dis_con, status, status2, cg[2];
    if (!read_word(SPM + 0x23c, &dis_con) || !read_word(SPM + 0x60c, &status) ||
        !read_word(SPM + 0x610, &status2)) {
        usb_say("regs: /dev/mem unavailable\n");
        return;
    }
    put("spm: DIS_PWR_CON=0x"); put_hex(dis_con, 8); put(" PWR_STATUS=0x"); put_hex(status, 8);
    put(" PWR_STATUS_2ND=0x"); put_hex(status2, 8); put("\n"); send(USB);
    if (!(status & status2 & 8)) { usb_say("display power domain off: no MMSYS read\n"); return; }
    if (!read_word(MMSYS + 0x100, &cg[0]) || !read_word(MMSYS + 0x110, &cg[1])) return;
    put("mmsys: CG_CON0=0x"); put_hex(cg[0], 8); put(" CG_CON1=0x"); put_hex(cg[1], 8);
    put(" (set = gated)\n"); send(USB);
    for (unsigned i = 0; i < sizeof(windows) / sizeof(windows[0]); ++i) {
        const struct window *w = &windows[i];
        if (w->gate_group != NO_GATE && cg[w->gate_group] & (1u << w->gate_bit)) {
            put("== "); put(w->name); put(": clock gated, not read\n"); send(USB);
            continue;
        }
        dump_window(w, watchdog);
    }
}

/* The MT6323 ISINK block (0x330-0x356), through the read-only regmap file: does LK light the
 * backlight from the PMIC? */
static void shell_isink(void)
{
    long fd = regmap_file[0] ? call5(SYS_OPEN, (long)regmap_file, O_RDONLY, 0, 0, 0) : -1;
    unsigned value;
    if (fd < 0) { usb_say("isink: regmap debugfs unavailable\n"); return; }
    usb_say("== pmic isink 0x330-0x356\n");
    for (unsigned reg = 0x330; reg <= 0x356; reg += 2) {
        put_hex(reg, 3); put("=");
        if (pmic_read(fd, reg, &value)) put_hex(value, 4);
        else put("????");
        put((reg - 0x330) % 16 == 14 || reg == 0x356 ? "\n" : "  ");
        send(USB);
    }
    call5(SYS_CLOSE, fd, 0, 0, 0, 0);
}

static int registers_command(const char *line, const char *argument, long watchdog)
{
    (void)argument;
    if (!same(line, "regs")) return 0;
    shell_regs(watchdog);
    shell_isink();
    return 1;
}

enum { FBIOGET_VSCREENINFO = 0x4600, FBIOGET_FSCREENINFO = 0x4602, PROT_WRITE = 2, BAR_SECONDS = 10 };
enum { FB_SAVE_BYTES = 480 * 360 * 4 };

static u8 fb_saved[FB_SAVE_BYTES];

/* White, yellow, cyan, green, magenta, red, blue, black as XRGB8888. */
static const u32 bar_colours[8] = {
    0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000,
};

static u32 to_pixel(u32 rgb, unsigned bpp)
{
    if (bpp == 32) return rgb;
    return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f); /* RGB565 */
}

static void show_bars(long watchdog)
{
    u32 var[40], fix[20];
    long fd = call5(SYS_OPEN, (long)"/dev/fb0", O_RDWR, 0, 0, 0);
    if (fd < 0) { put("y1-init: /dev/fb0 missing: no DRM frame buffer\n"); send(SCREEN | USB); return; }
    if (call5(SYS_IOCTL, fd, FBIOGET_VSCREENINFO, (long)var, 0, 0) != 0 ||
        call5(SYS_IOCTL, fd, FBIOGET_FSCREENINFO, (long)fix, 0, 0) != 0) {
        put("y1-init: frame buffer info unavailable\n"); send(SCREEN | USB);
        call5(SYS_CLOSE, fd, 0, 0, 0, 0);
        return;
    }
    /* fb_var_screeninfo: xres, yres at 0/4, bits_per_pixel at 24; fb_fix_screeninfo (32-bit):
     * smem_len at 20, line_length at 44. */
    unsigned width = var[0], height = var[1], bpp = var[6], stride = fix[11], bytes = stride * height;
    put("y1-init: DRM frame buffer "); put_number((int)width); put("x"); put_number((int)height);
    put(" "); put_number((int)bpp); put(" bpp, stride "); put_number((int)stride); put("\n"); send(SCREEN | USB);
    if ((bpp != 16 && bpp != 32) || bytes > FB_SAVE_BYTES || bytes > fix[5] || width == 0) {
        put("y1-init: unexpected frame buffer layout, no pattern\n"); send(SCREEN | USB);
        call5(SYS_CLOSE, fd, 0, 0, 0, 0);
        return;
    }
    long p = call6(SYS_MMAP2, 0, (long)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p < 0 && p > -4096) {
        put("y1-init: frame buffer mapping refused\n"); send(SCREEN | USB);
        call5(SYS_CLOSE, fd, 0, 0, 0, 0);
        return;
    }
    u8 *fb = (u8 *)p;
    for (unsigned i = 0; i < bytes; ++i) fb_saved[i] = fb[i];
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            u32 pixel = to_pixel(bar_colours[x * 8 / width], bpp);
            if (bpp == 32) *(u32 *)(fb + y * stride + 4 * x) = pixel;
            else *(unsigned short *)(fb + y * stride + 2 * x) = (unsigned short)pixel;
        }
    for (unsigned s = 0; s < BAR_SECONDS; ++s) { feed(watchdog); sleep_seconds(1); }
    for (unsigned i = 0; i < bytes; ++i) fb[i] = fb_saved[i];
    call5(SYS_MUNMAP, p, (long)bytes, 0, 0, 0);
    call5(SYS_CLOSE, fd, 0, 0, 0, 0);
    put("y1-init: colour bars shown for 10 s, screen restored\n"); send(SCREEN | USB);
}

static int bars_command(const char *line, const char *argument, long watchdog)
{
    (void)argument;
    if (!same(line, "bars")) return 0;
    show_bars(watchdog);
    return 1;
}

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
        put("y1-init: backlight "); put_number(level); put(r == (long)n ? "\n" : " refused\n"); send(SCREEN);
    }
    show_backlight();
    shell_isink();
}

static int backlight_command(const char *line, const char *argument, long watchdog)
{
    (void)watchdog;
    if (!same(line, "bl")) return 0;
    shell_backlight(argument);
    return 1;
}
