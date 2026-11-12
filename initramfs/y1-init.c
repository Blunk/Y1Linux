/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 PID1 (docs/knowledge/y1-hardware.md): prepares the Y2 root on the Y1's eMMC and hands over to
 * the Y2's busybox init, or keeps its own session with a USB shell when that fails (y1-session.h). Built into
 * the kernel's initramfs (initramfs/y1-production.list); `--selftest` runs the build's checks under QEMU.
 * ARM EABI numbers: upstream v6.18 arch/arm/tools/syscall.tbl. Entry: initramfs/start.S calls diag_start with
 * the initial stack.
 */
typedef long long s64;

enum {
    SYS_EXIT = 1, SYS_READ = 3, SYS_WRITE = 4, SYS_OPEN = 5, SYS_CLOSE = 6, SYS_MOUNT = 21,
    SYS_REBOOT = 88, SYS_CLOCK_NANOSLEEP_TIME64 = 407,
};
enum { CLOCK_MONOTONIC = 1, EINTR = 4, O_WRONLY = 1 };

struct timespec64 {
    s64 sec;
    s64 nsec;
};

static long call5(long number, long a, long b, long c, long d, long e)
{
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    register long r3 __asm__("r3") = d;
    register long r4 __asm__("r4") = e;
    register long r7 __asm__("r7") = number;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r7) : "memory", "cc");
    return r0;
}

static void write_all(const char *p, unsigned n)
{
    while (n) {
        long r = call5(SYS_WRITE, 1, (long)p, n, 0, 0);
        if (r == -EINTR) continue;
        if (r <= 0) return;
        p += r; n -= (unsigned)r;
    }
}

static void say(const char *p)
{
    unsigned n = 0;
    while (p[n]) ++n;
    write_all(p, n);
}

static void say_number(unsigned value)
{
    char digits[11];
    unsigned i = sizeof(digits);
    do {
        digits[--i] = (char)('0' + value % 10);
        value /= 10;
    } while (value && i);
    write_all(digits + i, sizeof(digits) - i);
}

static void sleep_seconds(unsigned seconds)
{
    struct timespec64 request = { seconds, 0 };
    while (call5(SYS_CLOCK_NANOSLEEP_TIME64, CLOCK_MONOTONIC, 0, (long)&request, (long)&request, 0) == -EINTR)
        ;
}

/* The first line of a /proc file, which is all this test shows of each. */
static void show_first_line(const char *label, const char *path)
{
    char buf[160];
    long fd = call5(SYS_OPEN, (long)path, 0, 0, 0, 0);
    long n = fd < 0 ? fd : call5(SYS_READ, fd, (long)buf, sizeof(buf) - 1, 0, 0);
    if (fd >= 0) call5(SYS_CLOSE, fd, 0, 0, 0, 0);
    say(label);
    if (n <= 0) { say("(unreadable)\n"); return; }
    unsigned end = 0;
    while (end < (unsigned)n && buf[end] != '\n') ++end;
    buf[end] = '\n';
    write_all(buf, end + 1);
}

static int is_selftest(long *stack)
{
    const char *arg = stack[0] > 1 ? (const char *)stack[2] : 0;
    const char *want = "--selftest";
    if (!arg) return 0;
    while (*arg && *arg == *want) { ++arg; ++want; }
    return *arg == *want;
}

#include "y1-storage.h"
#include "y1-session.h"
#include "y1-charging.h"
#include "y1-display.h"
#include "y1-root.h"
#include "y1-system.h"
#include "y1-gadget.h"
#include "y1-charge-mode.h"
#include "y1-console.h"
#include "y1-charge-screen.h"
#include "y1-recovery.h"

static void observe(void)
{
    console_to("/dev/ttyS0"); /* a quiet screen until the splash; the list carries the node */
    say("\ny1-init: Linux reached userspace on the Innioasis Y1.\n");
    /* Left to the kernel's boot handler, as on the Y2: the Y2's init never feeds it. */
    long watchdog = -1;
    say("watchdog: left to the kernel's boot handler\n");
    if (call5(SYS_MOUNT, (long)"proc", (long)"/proc", (long)"proc", 0, 0) == 0) {
        show_first_line("kernel: ", "/proc/version");
        show_first_line("memory: ", "/proc/meminfo");
    } else {
        say("proc: mount failed\n");
    }
    run_session(watchdog); /* never returns: busybox init takes PID 1, or the session keeps it */
}

void diag_start(long *stack)
{
    if (is_selftest(stack)) {
        if (!sha256_selftest()) { say("Y1INIT SELFTEST SHA-256 FAILED\n"); call5(SYS_EXIT, 1, 0, 0, 0, 0); }
        if (!telemetry_selftest()) { say("Y1INIT SELFTEST TELEMETRY FAILED\n"); call5(SYS_EXIT, 1, 0, 0, 0, 0); }
        if (!pack_temperature_selftest()) { say("Y1INIT SELFTEST PACK TEMPERATURE FAILED\n"); call5(SYS_EXIT, 1, 0, 0, 0, 0); }
        if (!recovery_selftest()) { say("Y1INIT SELFTEST RECOVERY FAILED\n"); call5(SYS_EXIT, 1, 0, 0, 0, 0); }
        say("Y1INIT SELFTEST OK\n");
        call5(SYS_EXIT, 0, 0, 0, 0, 0);
    }
    observe();
    for (;;) sleep_seconds(3600); /* not reached */
}
