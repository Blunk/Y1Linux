/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 seventh-boot additions (docs/planning/Y1-DISPLAY-PLAN.md, stage B), included after
 * y1-sixth-boot.h only when Y1_SEVENTH_BOOT is defined, so the earlier PID1 binaries stay byte-identical.
 * The console now runs on the MediaTek DRM driver's fbdev emulation instead of LK's frame buffer. A
 * test pattern (eight colour bars) proves scan-out through the new pipeline: PID1 saves the screen,
 * draws the bars for ten seconds, then restores it. The shell command `bars` repeats it.
 */
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
    if (fd < 0) { put("Y1 SEVENTH BOOT: /dev/fb0 missing: no DRM frame buffer\n"); send(SCREEN | USB); return; }
    if (call5(SYS_IOCTL, fd, FBIOGET_VSCREENINFO, (long)var, 0, 0) != 0 ||
        call5(SYS_IOCTL, fd, FBIOGET_FSCREENINFO, (long)fix, 0, 0) != 0) {
        put("Y1 SEVENTH BOOT: frame buffer info unavailable\n"); send(SCREEN | USB);
        call5(SYS_CLOSE, fd, 0, 0, 0, 0);
        return;
    }
    /* fb_var_screeninfo: xres, yres at 0/4, bits_per_pixel at 24; fb_fix_screeninfo (32-bit):
     * smem_len at 20, line_length at 44. */
    unsigned width = var[0], height = var[1], bpp = var[6], stride = fix[11], bytes = stride * height;
    put("Y1 SEVENTH BOOT: DRM frame buffer "); put_number((int)width); put("x"); put_number((int)height);
    put(" "); put_number((int)bpp); put(" bpp, stride "); put_number((int)stride); put("\n"); send(SCREEN | USB);
    if ((bpp != 16 && bpp != 32) || bytes > FB_SAVE_BYTES || bytes > fix[5] || width == 0) {
        put("Y1 SEVENTH BOOT: unexpected frame buffer layout, no pattern\n"); send(SCREEN | USB);
        call5(SYS_CLOSE, fd, 0, 0, 0, 0);
        return;
    }
    long p = call6(SYS_MMAP2, 0, (long)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p < 0 && p > -4096) {
        put("Y1 SEVENTH BOOT: frame buffer mapping refused\n"); send(SCREEN | USB);
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
    put("Y1 SEVENTH BOOT: colour bars shown for 10 s, screen restored\n"); send(SCREEN | USB);
}

static int seventh_command(const char *line, const char *argument, long watchdog)
{
    (void)argument;
    if (!same(line, "bars")) return 0;
    show_bars(watchdog);
    return 1;
}
