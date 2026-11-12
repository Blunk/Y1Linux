/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 PID1 hand-over (docs/planning/Y1-SYSTEM-PLAN.md): installs the Y1 overrides (initramfs
 * /y1-system) into the RAM overlay's upper layer before the overlay is mounted, then hands PID 1 to the Y2's
 * busybox init. It never opens /dev/watchdog: the kernel's boot handler keeps feeding it, as on the Y2. If any
 * step fails, PID1 keeps its own session and USB shell.
 */
enum { O_CREAT = 0100, O_TRUNC = 01000, MS_MOVE = 8192 };

struct override {
    const char *path; /* the same in /y1-system and in the root */
    unsigned mode;
};

static const char *const override_dirs[] = {
    "/mnt/upper/u/etc", "/mnt/upper/u/etc/init.d", "/mnt/upper/u/etc/default", "/mnt/upper/u/usr",
    "/mnt/upper/u/usr/libexec",
    "/mnt/upper/u/etc/y2linux",
};

/* SSH state where the Y2 keeps it (/etc/dropbear and /root/.ssh link into /data/ssh), in RAM until a
 * Y2DATA exists (docs/planning/Y1-NETWORK-PLAN.md). */
static const struct override private_dirs[] = {
    { "/data", 0755 },
    { "/data/ssh", 0755 },
    { "/data/ssh/host-keys", 0700 },
    { "/data/ssh/authorized_keys.d", 0700 },
};

static const struct override overrides[] = {
    { "/etc/inittab", 0644 },
    { "/etc/init.d/y1-services", 0644 },
    { "/etc/init.d/rcS", 0755 },
    { "/etc/init.d/rcK", 0755 },
    { "/etc/default/syslogd", 0644 },
    { "/usr/libexec/reborn-boot-services", 0755 },
    { "/etc/init.d/S50y1-dropbear", 0755 },
    { "/data/ssh/authorized_keys.d/authorized_keys", 0600 },
    /* Reborn's first UI (docs/planning/Y1-REBORN-PLAN.md, stage R2). */
    { "/etc/init.d/S90y1-reborn", 0755 },
    /* Reborn's audio profile for the Y1 card (stage R3). */
    { "/etc/y2linux/audio-enabled.json", 0644 },
    /* The power daemon with the Y1 battery profile and policy (docs/planning/Y1-POWER-PLAN.md, D2). */
    { "/etc/init.d/S03y1-data", 0755 },
    { "/etc/y2linux/battery-profile.json", 0644 },
    { "/etc/y2linux/power-policy.json", 0644 },
    /* MTP with one storage, /data/music (docs/planning/Y1-MTP-PLAN.md, M2). */
    { "/etc/init.d/S45y1-mtp", 0755 },
    /* Bluetooth: hci0 from the owner's record, then y2-radiod (docs/planning/Y1-BLUETOOTH-PLAN.md, B6). */
    { "/etc/init.d/S41y2-connectivity", 0755 },
    /* B7: the Y1's radio firmware set for y2-platform firmware (/data/firmware/mt6572). */
    { "/etc/y2linux/radio-firmware.json", 0644 },
    /* W6: the Y2's service daemon restricted to its network part (docs/planning/Y1-WIFI-PLAN.md §14); R4b: and
     * its update part, started by the Y1's S42 and reporting OTA off until R5 (docs/planning/Y1-RECOVERY-PLAN.md). */
    { "/etc/y2linux/service-policy.json", 0644 },
    { "/etc/init.d/S42y2-readiness", 0755 },
    { "/etc/y2linux/capabilities.json", 0644 },
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
    for (unsigned i = 0; i < sizeof(private_dirs) / sizeof(private_dirs[0]); ++i) {
        char dir[96];
        copy(dir, sizeof(dir), "/mnt/upper/u", private_dirs[i].path, 0);
        call5(SYS_MKDIR, (long)dir, private_dirs[i].mode, 0, 0, 0);
    }
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
    say("y1-init: handing over to the Y2 system (busybox init); shell on USB\n");
    for (long fd = 3; fd < 64; ++fd) call5(SYS_CLOSE, fd, 0, 0, 0, 0); /* PID1's USB port and the rest */
    usb_tty = -1;
    if (call5(SYS_CHDIR, (long)"/newroot", 0, 0, 0, 0) == 0 &&
        call5(SYS_MOUNT, (long)".", (long)"/", 0, MS_MOVE, 0) == 0 &&
        call5(SYS_CHROOT, (long)".", 0, 0, 0, 0) == 0 && call5(SYS_CHDIR, (long)"/", 0, 0, 0, 0) == 0)
        call5(SYS_EXECVE, (long)"/sbin/init", (long)argv, (long)envp, 0, 0);
    say("y1-init: hand-over failed; staying in the PID1 session\n");
}
