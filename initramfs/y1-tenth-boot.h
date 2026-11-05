/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 tenth-boot additions (docs/planning/Y1-SYSTEM-PLAN.md, milestone A), included after
 * y1-ninth-boot.h only when Y1_TENTH_BOOT is defined, so the earlier PID1 binaries stay byte-identical.
 * The test PID1 prepares the root as in the ninth boot, installs the Y1 overrides (initramfs
 * /y1-system) into the RAM overlay's upper layer before the overlay is mounted, then hands PID 1 to the
 * Y2's busybox init. It never opens /dev/watchdog: the kernel's boot handler keeps feeding it, as on the
 * Y2. If any step fails, PID1 keeps its own session and USB shell.
 */
enum { O_CREAT = 0100, O_TRUNC = 01000, MS_MOVE = 8192 };

struct override {
    const char *path; /* the same in /y1-system and in the root */
    unsigned mode;
};

static const char *const override_dirs[] = {
    "/mnt/upper/u/etc", "/mnt/upper/u/etc/init.d", "/mnt/upper/u/etc/default", "/mnt/upper/u/usr",
    "/mnt/upper/u/usr/libexec",
};

static const struct override overrides[] = {
    { "/etc/inittab", 0644 },
    { "/etc/init.d/y1-services", 0644 },
    { "/etc/init.d/rcS", 0755 },
    { "/etc/init.d/rcK", 0755 },
    { "/etc/default/syslogd", 0644 },
    { "/usr/libexec/reborn-boot-services", 0755 },
};

static int install_file(const char *path, unsigned mode)
{
    char from[96], to[96];
    copy(from, sizeof(from), "/y1-system", path, 0);
    copy(to, sizeof(to), "/mnt/upper/u", path, 0);
    long in = call5(SYS_OPEN, (long)from, O_RDONLY, 0, 0, 0);
    long n = in < 0 ? in : call5(SYS_READ, in, (long)chunk, CHUNK, 0, 0);
    if (in >= 0) call5(SYS_CLOSE, in, 0, 0, 0, 0);
    if (n <= 0 || n == CHUNK) return 0;
    long out = call5(SYS_OPEN, (long)to, O_WRONLY | O_CREAT | O_TRUNC, mode, 0, 0);
    long w = out < 0 ? out : call5(SYS_WRITE, out, (long)chunk, n, 0, 0);
    if (out >= 0) call5(SYS_CLOSE, out, 0, 0, 0, 0);
    return w == n;
}

/* Into the empty upper layer, before the overlay is mounted on it. 1 when every file is in place. */
static int install_y1_overrides(void)
{
    for (unsigned i = 0; i < sizeof(override_dirs) / sizeof(override_dirs[0]); ++i)
        call5(SYS_MKDIR, (long)override_dirs[i], 0755, 0, 0, 0);
    for (unsigned i = 0; i < sizeof(overrides) / sizeof(overrides[0]); ++i)
        if (!install_file(overrides[i].path, overrides[i].mode)) {
            put("rootfs: Y1 override "); put(overrides[i].path); put(" not installed\n"); send(SCREEN | USB);
            return 0;
        }
    return 1;
}

/* switch_root: the prepared root becomes / and the Y2's init becomes PID 1. Returns only on failure. */
static void switch_to_y2(void)
{
    static const char *const argv[] = { "init", 0 };
    static const char *const envp[] = { "HOME=/", "TERM=linux", "PATH=/usr/sbin:/usr/bin:/sbin:/bin", 0 };
    say("Y1 TENTH BOOT: handing over to the Y2 system (busybox init); shell on USB\n");
    for (long fd = 3; fd < 64; ++fd) call5(SYS_CLOSE, fd, 0, 0, 0, 0); /* PID1's USB port and the rest */
    usb_tty = -1;
    if (call5(SYS_CHDIR, (long)"/newroot", 0, 0, 0, 0) == 0 &&
        call5(SYS_MOUNT, (long)".", (long)"/", 0, MS_MOVE, 0) == 0 &&
        call5(SYS_CHROOT, (long)".", 0, 0, 0, 0) == 0 && call5(SYS_CHDIR, (long)"/", 0, 0, 0, 0) == 0)
        call5(SYS_EXECVE, (long)"/sbin/init", (long)argv, (long)envp, 0, 0);
    say("Y1 TENTH BOOT: hand-over failed; staying in the Y1 test session\n");
}
