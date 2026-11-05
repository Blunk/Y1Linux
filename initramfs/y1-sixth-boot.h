/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 sixth-boot additions (docs/planning/Y1-DISPLAY-PLAN.md, stage A), included after
 * y1-fifth-boot.h only when Y1_SIXTH_BOOT is defined, so the earlier PID1 binaries stay byte-identical.
 * A read-only dump of LK's display registers: each window is mapped PROT_READ from /dev/mem and read
 * with 32-bit loads. The display power domain is checked first and each block's MMSYS clock gate
 * before its window, because a read from an unpowered or unclocked block can stall the bus. Nothing
 * is written and no interrupt is acknowledged.
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

static int sixth_command(const char *line, const char *argument, long watchdog)
{
    (void)argument;
    if (!same(line, "regs")) return 0;
    shell_regs(watchdog);
    shell_isink();
    return 1;
}
