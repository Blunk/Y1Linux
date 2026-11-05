/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 first-boot PID1: shows on the player's screen that Linux reached userspace, then reboots
 * after the observation window, so the owner can restore BOOTIMG through the preloader (the Y1 has no
 * power button). It holds /dev/watchdog open and feeds it on every heartbeat, so a hang from here on
 * ends in a reset within the 15 s timeout. No storage or network access. ARM EABI numbers: upstream v6.18
 * arch/arm/tools/syscall.tbl. Entry: initramfs/start.S calls diag_start with the initial stack.
 */
typedef long long s64;

enum {
    SYS_EXIT = 1, SYS_READ = 3, SYS_WRITE = 4, SYS_OPEN = 5, SYS_CLOSE = 6, SYS_MOUNT = 21,
    SYS_REBOOT = 88, SYS_CLOCK_NANOSLEEP_TIME64 = 407,
};
enum { CLOCK_MONOTONIC = 1, EINTR = 4, O_WRONLY = 1 };
enum { HEARTBEATS = 24, HEARTBEAT_SECONDS = 5 };

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

/* Opening starts the watchdog if LK left it stopped; any write feeds it. Never closed. */
static long arm_watchdog(void)
{
    long fd = call5(SYS_OPEN, (long)"/dev/watchdog", O_WRONLY, 0, 0, 0);
    say(fd < 0 ? "watchdog: unavailable\n" : "watchdog: armed, fed every heartbeat\n");
    return fd;
}

#ifdef Y1_SECOND_BOOT
#include "y1-second-boot.h"
#endif
#ifdef Y1_THIRD_BOOT
#include "y1-third-boot.h"
#endif

static void observe(void)
{
    say("\nY1 FIRST BOOT: Linux reached userspace on the Innioasis Y1.\n");
    long watchdog = arm_watchdog();
    if (call5(SYS_MOUNT, (long)"proc", (long)"/proc", (long)"proc", 0, 0) == 0) {
        show_first_line("kernel: ", "/proc/version");
        show_first_line("memory: ", "/proc/meminfo");
    } else {
        say("proc: mount failed\n");
    }
#if defined(Y1_THIRD_BOOT)
    third_boot(watchdog);
#elif defined(Y1_SECOND_BOOT)
    second_boot(watchdog);
#endif
    for (unsigned beat = 1; beat <= HEARTBEATS; ++beat) {
        sleep_seconds(HEARTBEAT_SECONDS);
        if (watchdog >= 0) call5(SYS_WRITE, watchdog, (long)"k", 1, 0, 0);
        say("Y1 FIRST BOOT: alive ");
        say_number(beat * HEARTBEAT_SECONDS);
        say(" s\n");
    }
}

void diag_start(long *stack)
{
    if (is_selftest(stack)) {
#ifdef Y1_SECOND_BOOT
        if (!sha256_selftest()) { say("Y1INIT SELFTEST SHA-256 FAILED\n"); call5(SYS_EXIT, 1, 0, 0, 0, 0); }
#endif
#ifdef Y1_THIRD_BOOT
        if (!third_boot_selftest()) { say("Y1INIT SELFTEST TELEMETRY FAILED\n"); call5(SYS_EXIT, 1, 0, 0, 0, 0); }
#endif
        say("Y1INIT SELFTEST OK\n");
        call5(SYS_EXIT, 0, 0, 0, 0, 0);
    }
    observe();
    say("Y1 FIRST BOOT: rebooting now; restore BOOTIMG through the preloader.\n");
    sleep_seconds(2);
    /* LINUX_REBOOT_MAGIC1, MAGIC2, CMD_RESTART: the MediaTek watchdog's restart handler. */
    call5(SYS_REBOOT, (long)0xfee1dead, 0x28121969, 0x01234567, 0, 0);
    say("Y1 FIRST BOOT: reboot refused; the unfed watchdog resets the player\n");
    for (;;) sleep_seconds(3600);
}
