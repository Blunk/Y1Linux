/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 PID1 charging screen (docs/planning/Y1-CHARGE-MODE-PLAN.md): the charge-only mode draws the
 * Y2's charging screen (tools/production/offline-charge.c): the same artwork rendered by Reborn
 * (tools/graphics/offline-charge-art.h), layout and colours, the ring sweeping while charging and full when
 * charged, the title (Charging, Charged, Charging paused) and the hint (Hold Power to turn on, Release to
 * start, Battery too low to start). The screen goes dark 8 s after the last press and a press lights it, as on
 * the Y2. Freestanding: the ring's coverage uses integer sixths of a pixel and 30-degree sectors instead of the
 * Y2's floats; /dev/fb0 is the DRM frame buffer, XRGB8888 480x360.
 */
#include "../tools/graphics/offline-charge-art.h"

enum { KDSETMODE = 0x4b3a, KD_TEXT = 0, KD_GRAPHICS = 1, FBIOBLANK = 0x4611, FB_BLANK_UNBLANK = 0,
       FB_BLANK_POWERDOWN = 4, CHARGE_SCREEN_MS = 8000, CHARGE_FRAME_MS = 500 };

static u8 *chg_fb;
static unsigned chg_stride, chg_bytes, chg_lit_on;
static long chg_fd = -1, chg_tty = -1;
static u32 chg_show_until;
static char chg_light[8] = "32";

static void chg_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned rgb)
{
    if (!chg_fb || x + w > 480 || y + h > 360) return;
    for (unsigned row = y; row < y + h; ++row)
        for (unsigned col = x; col < x + w; ++col) *(u32 *)(chg_fb + row * chg_stride + 4 * col) = rgb;
}

/* One RGB888 run-length image rendered by Reborn (as offline-charge.c's blit). */
static void chg_blit(const chg_image *image)
{
    unsigned x = 0, y = 0, at = image->offset, end = image->offset + image->length;
    while (at + 3 < end && y < image->h) {
        unsigned count = chg_art_rle[at];
        unsigned rgb = (unsigned)chg_art_rle[at + 1] << 16 | (unsigned)chg_art_rle[at + 2] << 8 | chg_art_rle[at + 3];
        at += 4;
        while (count--) {
            chg_rect(image->x + x, image->y + y, 1, 1, rgb);
            if (++x == image->w) { x = 0; y++; }
        }
    }
}

/* The 30-degree sector (0..11, clockwise from 12 o'clock) of a point right of (x) and above (up) the centre:
 * the integer form of offline-charge.c's diamond angle against its 30-degree steps (tan 30 = 0.577,
 * tan 60 = 1.732). */
static unsigned chg_sector(int x, int up)
{
    int a, b;
    unsigned base;
    if (x >= 0 && up > 0) { base = 0; a = x; b = up; }
    else if (x > 0) { base = 3; a = -up; b = x; }
    else if (up < 0) { base = 6; a = -x; b = -up; }
    else { base = 9; a = up; b = -x; }
    long long a1000 = (long long)a * 1000;
    return base + (a1000 < (long long)b * 577 ? 0 : a1000 < (long long)b * 1732 ? 1 : 2);
}

/* The lit part of the ring, `twelfths` of a turn from 12 o'clock, 3x3 coverage at its edges (lit_ring). */
static void chg_ring(unsigned twelfths)
{
    enum { CX6 = 6 * (int)CHG_CX, CY6 = 6 * (int)CHG_CY, OUT6 = 6 * (int)CHG_OUTER + 3, IN6 = 6 * (int)CHG_INNER - 3 };
    if (!twelfths) return;
    int x0 = (int)CHG_CX - (int)CHG_OUTER - 2, x1 = (int)CHG_CX + (int)CHG_OUTER + 3;
    int y0 = (int)CHG_CY - (int)CHG_OUTER - 2, y1 = (int)CHG_CY + (int)CHG_OUTER + 3;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            unsigned hits = 0;
            for (int sy = 0; sy < 3; ++sy)
                for (int sx = 0; sx < 3; ++sx) {
                    int px = 6 * x + 2 * sx + 1 - CX6, py = 6 * y + 2 * sy + 1 - CY6;
                    int d2 = px * px + py * py;
                    if (d2 > OUT6 * OUT6 || d2 < IN6 * IN6) continue;
                    if (twelfths >= 12 || chg_sector(px, -py) < twelfths) hits++;
                }
            if (!hits) continue;
            unsigned a = hits * 255 / 9, rgb = 0;
            for (unsigned shift = 0; shift <= 16; shift += 8)
                rgb |= ((((CHG_TRACK >> shift) & 255) * (255 - a) + ((CHG_LIT >> shift) & 255) * a + 127) / 255) << shift;
            chg_rect((unsigned)x, (unsigned)y, 1, 1, rgb);
        }
}

/* status: BAT0/status. held: the centre button down for at least 2 s; ready: three samples at 3.4 V or more. */
static void chg_draw(const char *status, int held, int ready, u32 now)
{
    int charging = same(status, "Charging"), full = same(status, "Full");
    chg_rect(0, 0, 480, 360, CHG_BG);
    chg_blit(&chg_images[CHG_BASE]);
    chg_ring(full ? 12 : charging ? 1 + now / 500 % 12 : 0);
    chg_blit(&chg_images[CHG_TITLE(full ? 1 : charging ? 0 : 2)]);
    chg_blit(&chg_images[CHG_HINT(held ? (ready ? 1 : 2) : 0)]);
}

static void chg_light_on(int on)
{
    if (!chg_fb || on == (int)chg_lit_on) return;
    if (on) {
        call5(SYS_IOCTL, chg_fd, FBIOBLANK, FB_BLANK_UNBLANK, 0, 0);
        set_brightness(chg_light);
    } else {
        set_brightness("0");
        call5(SYS_IOCTL, chg_fd, FBIOBLANK, FB_BLANK_POWERDOWN, 0, 0);
    }
    chg_lit_on = (unsigned)on;
}

/* The frame buffer for the charging screen; 0 (the text mode stays) when it is not the expected one. */
static int charge_screen_open(const char *light)
{
    u32 var[40], fix[20];
    copy(chg_light, sizeof(chg_light), light, 0, 0);
    chg_fd = call5(SYS_OPEN, (long)"/dev/fb0", O_RDWR, 0, 0, 0);
    if (chg_fd < 0) return 0;
    if (call5(SYS_IOCTL, chg_fd, FBIOGET_VSCREENINFO, (long)var, 0, 0) != 0 ||
        call5(SYS_IOCTL, chg_fd, FBIOGET_FSCREENINFO, (long)fix, 0, 0) != 0 ||
        var[0] != 480 || var[1] != 360 || var[6] != 32 || fix[11] < 1920 || fix[11] * 360 > fix[5]) {
        call5(SYS_CLOSE, chg_fd, 0, 0, 0, 0);
        chg_fd = -1;
        return 0;
    }
    chg_stride = fix[11];
    chg_bytes = chg_stride * 360;
    long p = call6(SYS_MMAP2, 0, (long)chg_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, chg_fd, 0);
    if (p < 0 && p > -4096) { call5(SYS_CLOSE, chg_fd, 0, 0, 0, 0); chg_fd = -1; return 0; }
    chg_fb = (u8 *)p;
    chg_tty = call5(SYS_OPEN, (long)"/dev/tty0", O_RDWR | O_NOCTTY, 0, 0, 0);
    if (chg_tty >= 0) call5(SYS_IOCTL, chg_tty, KDSETMODE, KD_GRAPHICS, 0, 0);
    chg_lit_on = 1;
    chg_show_until = CHARGE_SCREEN_MS;
    return 1;
}

/* One 100 ms tick of the charge-only mode: a frame every 500 ms while lit; dark 8 s after the last press. */
static void charge_screen_tick(u32 now, int pressed, int held, int ready)
{
    char status[32];
    if (!chg_fb) return;
    if (pressed) chg_show_until = now + CHARGE_SCREEN_MS;
    chg_light_on(now < chg_show_until);
    if (!chg_lit_on || (now % CHARGE_FRAME_MS && !pressed)) return;
    if (read_text(Y1_BATTERY "/status", status, sizeof(status)) <= 0) copy(status, sizeof(status), "Unknown", 0, 0);
    chg_draw(status, held, ready, now);
}

static void charge_screen_close(void)
{
    if (!chg_fb) return;
    chg_light_on(1);
    chg_rect(0, 0, 480, 360, 0);
    if (chg_tty >= 0) { call5(SYS_IOCTL, chg_tty, KDSETMODE, KD_TEXT, 0, 0); call5(SYS_CLOSE, chg_tty, 0, 0, 0, 0); }
    call5(SYS_MUNMAP, (long)chg_fb, chg_bytes, 0, 0, 0);
    call5(SYS_CLOSE, chg_fd, 0, 0, 0, 0);
    chg_fb = 0;
    chg_fd = chg_tty = -1;
}
