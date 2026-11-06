/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 twenty-second-boot additions (docs/planning/Y1-MTP-PLAN.md, stage M1), included after
 * y1-tenth-boot.h only when Y1_TWENTY_SECOND_BOOT is defined, so the earlier PID1 binaries stay byte-identical.
 * The legacy g_cdc gadget is gone from the kernel; PID1 builds the same functions through configfs before its
 * USB shell: ACM (/dev/ttyGS0, the rescue shell, later inittab's getty) and ECM (usb0), the Y2's identity
 * (0525:a4aa, Y2Linux), one configuration at MaxPower 500 (the Y1 charges at its 450 mA ceiling only from a PC
 * that configured a 500 mA gadget). Fixed MAC addresses keep the PC's interface stable. S45y1-mtp adds MTP later.
 */
enum { SYS_SYMLINK = 83 };

#define Y1_GADGET "/sys/kernel/config/usb_gadget/y1"

/* The gadget's progress into the kernel log as well, so it can be read over SSH afterwards. */
static void gadget_log(const char *a, const char *b, const char *c)
{
    char line[160];
    unsigned n = 0;
    copy(line, sizeof(line), a, b, c);
    while (line[n]) ++n;
    long fd = call5(SYS_OPEN, (long)"/dev/kmsg", O_WRONLY, 0, 0, 0);
    if (fd >= 0) { call5(SYS_WRITE, fd, (long)line, n, 0, 0); call5(SYS_CLOSE, fd, 0, 0, 0, 0); }
}

static int gadget_write(const char *name, const char *value)
{
    char path[112];
    unsigned n = 0;
    copy(path, sizeof(path), Y1_GADGET "/", name, 0);
    while (value[n]) ++n;
    long fd = call5(SYS_OPEN, (long)path, O_WRONLY, 0, 0, 0);
    long r = fd < 0 ? fd : call5(SYS_WRITE, fd, (long)value, n, 0, 0);
    if (fd >= 0) call5(SYS_CLOSE, fd, 0, 0, 0, 0);
    if (r == (long)n) return 1;
    put("usb gadget: "); put(name); put(" refused "); put_number((int)r); put("\n"); send(SCREEN);
    gadget_log("y1 usb gadget: write refused: ", name, value);
    return 0;
}

static void gadget_mkdir(const char *name)
{
    char path[112];
    copy(path, sizeof(path), Y1_GADGET "/", name, 0);
    call5(SYS_MKDIR, (long)path, 0755, 0, 0, 0);
}

static int gadget_link(const char *function)
{
    char from[112], to[112];
    copy(from, sizeof(from), Y1_GADGET "/functions/", function, 0);
    copy(to, sizeof(to), Y1_GADGET "/configs/c.1/", function, 0);
    if (call5(SYS_SYMLINK, (long)from, (long)to, 0, 0, 0) == 0) return 1;
    put("usb gadget: link "); put(function); put(" failed\n"); send(SCREEN);
    gadget_log("y1 usb gadget: link failed: ", function, 0);
    return 0;
}

/* The controller's name from udc[] ("/sys/class/udc/<name>/state"), then the gadget bound to it. */
static void usb_gadget_setup(void)
{
    char name[48];
    unsigned i = 0;
    const char *p = udc + 15;
    if (!udc[0]) return;
    while (p[i] && p[i] != '/' && i < sizeof(name) - 1) { name[i] = p[i]; ++i; }
    name[i] = 0;
    if (call5(SYS_MOUNT, (long)"configfs", (long)"/sys/kernel/config", (long)"configfs", MS_SILENT, 0) != 0) {
        say("configfs: mount failed\n");
        gadget_log("y1 usb gadget: configfs mount failed", 0, 0);
    }
    call5(SYS_MKDIR, (long)Y1_GADGET, 0755, 0, 0, 0);
    static const char *const dirs[] = { "strings/0x409", "configs/c.1", "configs/c.1/strings/0x409",
                                        "functions/acm.GS0", "functions/ecm.usb0" };
    for (unsigned d = 0; d < sizeof(dirs) / sizeof(dirs[0]); ++d) gadget_mkdir(dirs[d]);
    int ok = gadget_write("idVendor", "0x0525") && gadget_write("idProduct", "0xa4aa") &&
             gadget_write("bcdUSB", "0x0200") &&
             gadget_write("strings/0x409/manufacturer", "Y2Linux") &&
             gadget_write("strings/0x409/product", "Y1") &&
             gadget_write("strings/0x409/serialnumber", "Y1LINUX-DEV-01") &&
             gadget_write("configs/c.1/MaxPower", "500") && gadget_write("configs/c.1/bmAttributes", "0x80") &&
             gadget_write("configs/c.1/strings/0x409/configuration", "Y1") &&
             gadget_write("functions/ecm.usb0/dev_addr", "02:59:31:00:00:01") &&
             gadget_write("functions/ecm.usb0/host_addr", "02:59:31:00:00:02") &&
             gadget_link("ecm.usb0") && gadget_link("acm.GS0") && gadget_write("UDC", name);
    put(ok ? "usb gadget: ECM + ACM on " : "usb gadget: NOT bound to "); put(name); put("\n"); send(SCREEN);
    gadget_log(ok ? "y1 usb gadget: ECM + ACM bound to " : "y1 usb gadget: NOT bound to ", name, 0);
}
