/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 ninth-boot additions (docs/planning/Y1-ROOTFS-PLAN.md, step 1), included after
 * y1-eighth-boot.h only when Y1_NINTH_BOOT is defined, so the earlier PID1 binaries stay byte-identical.
 * The Y2's Buildroot root filesystem (Y2ROOT) from the eMMC's ANDROID partition, where the Y2 keeps it
 * too, with nothing written by Linux: the partition through a read-only loop device over mmcblk0 (Linux
 * does not parse MediaTek's partition table: ANDROID is scatter 0x4b40000 - 0x600000 = physical
 * 0x4540000, 0x28a00000 bytes), ext4 `ro,noload` (no journal replay), a tmpfs overlay on top. `rootsh` runs the image's /bin/sh in a chroot on the USB serial port;
 * PID1 keeps the watchdog and stops reading the port until that shell exits. The Y2's init never runs.
 */
enum { SYS_FORK = 2, SYS_EXECVE = 11, SYS_CHDIR = 12, SYS_MKDIR = 39, SYS_DUP2 = 63, SYS_SETSID = 66,
       SYS_CHROOT = 61, SYS_WAIT4 = 114 };
enum { MS_RDONLY = 1, MS_NOSUID = 2, MS_NODEV = 4, MS_NOEXEC = 8, MS_NOATIME = 1024, MS_BIND = 4096 };
enum { LOOP_CTL_GET_FREE = 0x4c82, LOOP_CONFIGURE = 0x4c0a, LO_FLAGS_READ_ONLY = 1, TIOCSCTTY = 0x540e,
       WNOHANG = 1 };

enum { ANDROID_OFFSET = 0x4540000, ANDROID_SIZE = 0x28a00000 };
static int root_ready;
static long root_shell = -1; /* pid of the chroot shell while it runs */

/* struct loop_config: fd, block_size, struct loop_info64 (232 bytes; lo_flags at 52), __reserved[8]. */
struct loop_config {
    u32 fd, block_size;
    u8 info[232];
    u64 reserved[8];
};

static int mount_step(const char *source, const char *target, const char *type, unsigned long flags,
                      const char *data)
{
    long r = call5(SYS_MOUNT, (long)source, (long)target, (long)type, (long)flags, (long)data);
    if (r == 0) return 1;
    put("rootfs: mount "); put(target); put(" failed "); put_number((int)r); put("\n"); send(SCREEN | USB);
    return 0;
}

/* The ANDROID partition, read-only, on a free loop device; its path in device[], 1 on success. */
static int attach_image(char device[16])
{
    struct loop_config config;
    u8 *raw = (u8 *)&config;
    long image = call5(SYS_OPEN, (long)"/dev/mmcblk0", O_RDONLY, 0, 0, 0);
    long control = call5(SYS_OPEN, (long)"/dev/loop-control", O_RDWR, 0, 0, 0);
    long number = control < 0 ? control : call5(SYS_IOCTL, control, LOOP_CTL_GET_FREE, 0, 0, 0);
    if (control >= 0) call5(SYS_CLOSE, control, 0, 0, 0, 0);
    if (image < 0 || number < 0 || number > 9) {
        put("rootfs: "); put(image < 0 ? "no eMMC" : "no free loop device"); put("\n");
        send(SCREEN | USB);
        if (image >= 0) call5(SYS_CLOSE, image, 0, 0, 0, 0);
        return 0;
    }
    copy(device, 16, "/dev/loop", 0, 0);
    device[9] = (char)('0' + number);
    device[10] = 0;
    long loop = call5(SYS_OPEN, (long)device, O_RDONLY, 0, 0, 0);
    for (unsigned i = 0; i < sizeof(config); ++i) raw[i] = 0;
    config.fd = (u32)image;
    *(u64 *)(config.info + 24) = ANDROID_OFFSET; /* lo_offset */
    *(u64 *)(config.info + 32) = ANDROID_SIZE;   /* lo_sizelimit */
    *(u32 *)(config.info + 52) = LO_FLAGS_READ_ONLY;
    long r = loop < 0 ? loop : call5(SYS_IOCTL, loop, LOOP_CONFIGURE, (long)&config, 0, 0);
    call5(SYS_CLOSE, image, 0, 0, 0, 0);
    if (loop >= 0) call5(SYS_CLOSE, loop, 0, 0, 0, 0);
    if (r != 0) { put("rootfs: loop setup failed "); put_number((int)r); put("\n"); send(SCREEN | USB); return 0; }
    return 1;
}

#ifdef Y1_TWENTY_FIRST_BOOT
/* Y2DATA (docs/planning/Y1-DATA-PLAN.md, stage B2): USRDATA (scatter 0x4d540000 - 0x600000 = physical
 * 0x4cf40000, 2 GiB) read-write on a free loop device, for S03y1-data to check and mount. The eMMC itself must
 * then be writable, so this happens only when the device tree carries the eMMC guard's window, exactly USRDATA
 * (sectors 0x267a00 + 0x400000); the kernel refuses every other eMMC write. Never formats anything. */
enum { USRDATA_OFFSET = 0x4cf40000, USRDATA_SIZE = 0x80000000 };

static void attach_data(void)
{
    static const u8 window[8] = { 0x00, 0x26, 0x7a, 0x00, 0x00, 0x40, 0x00, 0x00 };
    u8 found[9];
    struct loop_config config;
    u8 *raw = (u8 *)&config;
    int zero = 0;
    long fd = call5(SYS_OPEN, (long)"/sys/firmware/devicetree/base/soc/mmc@11120000/innioasis,emmc-write-window",
                    O_RDONLY, 0, 0, 0);
    long n = fd < 0 ? fd : call5(SYS_READ, fd, (long)found, sizeof(found), 0, 0);
    if (fd >= 0) call5(SYS_CLOSE, fd, 0, 0, 0, 0);
    int match = n == 8;
    for (unsigned i = 0; match && i < 8; ++i) match = found[i] == window[i];
    if (!match) { put("data: no eMMC guard for USRDATA, Y2DATA not attached\n"); send(SCREEN | USB); return; }
    long disk = call5(SYS_OPEN, (long)"/dev/mmcblk0", O_RDONLY, 0, 0, 0);
    long r = disk < 0 ? disk : call5(SYS_IOCTL, disk, BLKROSET, (long)&zero, 0, 0);
    if (disk >= 0) call5(SYS_CLOSE, disk, 0, 0, 0, 0);
    disk = r < 0 ? r : call5(SYS_OPEN, (long)"/dev/mmcblk0", O_RDWR, 0, 0, 0);
    long control = call5(SYS_OPEN, (long)"/dev/loop-control", O_RDWR, 0, 0, 0);
    long number = control < 0 ? control : call5(SYS_IOCTL, control, LOOP_CTL_GET_FREE, 0, 0, 0);
    if (control >= 0) call5(SYS_CLOSE, control, 0, 0, 0, 0);
    if (disk < 0 || number < 0 || number > 9) {
        put("data: "); put(disk < 0 ? "eMMC not writable" : "no free loop device"); put("\n"); send(SCREEN | USB);
        if (disk >= 0) call5(SYS_CLOSE, disk, 0, 0, 0, 0);
        return;
    }
    char device[16];
    copy(device, sizeof(device), "/dev/loop", 0, 0);
    device[9] = (char)('0' + number);
    device[10] = 0;
    long loop = call5(SYS_OPEN, (long)device, O_RDWR, 0, 0, 0);
    for (unsigned i = 0; i < sizeof(config); ++i) raw[i] = 0;
    config.fd = (u32)disk;
    *(u64 *)(config.info + 24) = USRDATA_OFFSET; /* lo_offset */
    *(u64 *)(config.info + 32) = USRDATA_SIZE;   /* lo_sizelimit */
    r = loop < 0 ? loop : call5(SYS_IOCTL, loop, LOOP_CONFIGURE, (long)&config, 0, 0);
    call5(SYS_CLOSE, disk, 0, 0, 0, 0);
    if (loop >= 0) call5(SYS_CLOSE, loop, 0, 0, 0, 0);
    if (r != 0) { put("data: loop setup failed "); put_number((int)r); put("\n"); send(SCREEN | USB); return; }
    put("data: USRDATA on "); put(device); put(", writable behind the eMMC guard\n");
    send(SCREEN | USB);
}
#endif

static void prepare_root(void)
{
    char device[16];
    static const char *const dirs[] = { "/mnt", "/mnt/image", "/mnt/upper", "/newroot" };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) call5(SYS_MKDIR, (long)dirs[i], 0755, 0, 0, 0);
    if (!attach_image(device)) return;
    if (!mount_step(device, "/mnt/image", "ext4", MS_RDONLY | MS_NOATIME, "noload")) return;
    if (!mount_step("tmpfs", "/mnt/upper", "tmpfs", MS_NOSUID | MS_NODEV, "size=32m,mode=0755")) return;
    call5(SYS_MKDIR, (long)"/mnt/upper/u", 0755, 0, 0, 0);
    call5(SYS_MKDIR, (long)"/mnt/upper/w", 0755, 0, 0, 0);
#ifdef Y1_TENTH_BOOT
    if (!install_y1_overrides()) return;
#endif
    if (!mount_step("overlay", "/newroot", "overlay", 0,
                    "lowerdir=/mnt/image,upperdir=/mnt/upper/u,workdir=/mnt/upper/w"))
        return;
    if (!mount_step("proc", "/newroot/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, 0) ||
        !mount_step("sysfs", "/newroot/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, 0) ||
        !mount_step("/dev", "/newroot/dev", 0, MS_BIND, 0))
        return;
#ifdef Y1_TWENTY_FIRST_BOOT
    attach_data();
#endif
    root_ready = 1;
    put("rootfs: Y2ROOT (eMMC ANDROID) on "); put(device); put(", read-only below a RAM overlay; `rootsh` on USB\n");
    send(SCREEN | USB);
}

static void start_root_shell(void)
{
    if (!root_ready) { usb_say("rootsh: no root filesystem (see the rootfs line on screen)\n"); return; }
    long pid = call5(SYS_FORK, 0, 0, 0, 0, 0);
    if (pid < 0) { usb_say("rootsh: fork failed\n"); return; }
    if (pid == 0) {
        static const char *const argv[] = { "sh", "-l", 0 };
        static const char *const envp[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", "TERM=vt100", 0 };
        call5(SYS_SETSID, 0, 0, 0, 0, 0);
        long tty = call5(SYS_OPEN, (long)"/dev/ttyGS0", O_RDWR, 0, 0, 0);
        if (tty < 0) call5(SYS_EXIT, 1, 0, 0, 0, 0);
        call5(SYS_IOCTL, tty, TIOCSCTTY, 0, 0, 0);
        for (long fd = 0; fd < 3; ++fd) call5(SYS_DUP2, tty, fd, 0, 0, 0);
        for (long fd = 3; fd < 64; ++fd) call5(SYS_CLOSE, fd, 0, 0, 0, 0); /* not the watchdog, not PID1's port */
        if (call5(SYS_CHROOT, (long)"/newroot", 0, 0, 0, 0) != 0 || call5(SYS_CHDIR, (long)"/", 0, 0, 0, 0) != 0)
            call5(SYS_EXIT, 1, 0, 0, 0, 0);
        call5(SYS_EXECVE, (long)"/bin/sh", (long)argv, (long)envp, 0, 0);
        call5(SYS_EXIT, 127, 0, 0, 0, 0);
    }
    root_shell = pid;
    say("Y1 NINTH BOOT: Y2 root shell running on USB\n");
}

/* While the chroot shell owns the serial port, PID1 only reaps it; then the y1# prompt returns. */
static int root_shell_running(void)
{
    int status;
    if (root_shell < 0) return 0;
    if (call5(SYS_WAIT4, root_shell, (long)&status, WNOHANG, 0, 0) == 0) return 1;
    root_shell = -1;
    /* wait status: low 7 bits the terminating signal, else the exit code in bits 8-15. */
    put("Y1 NINTH BOOT: Y2 root shell ended, ");
    if (status & 0x7f) { put("signal "); put_number(status & 0x7f); }
    else { put("exit "); put_number((status >> 8) & 0xff); }
    put("\n");
    send(SCREEN | USB);
    usb_say("y1# ");
    return 0;
}

static int ninth_command(const char *line, const char *argument, long watchdog)
{
    (void)argument; (void)watchdog;
    if (!same(line, "rootsh")) return 0;
    start_root_shell();
    return 1;
}
