/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 PID1 quiet boot (docs/planning/Y1-BOOT-SPLASH-PLAN.md), as on the Y2: PID1's routine text goes
 * to the serial console (ttyS0) only and the screen stays dark until reborn-splash (started by
 * reborn-boot-services, as on the Y2) and then Reborn take it. The screen gets PID1's text back for the
 * charge-only mode and whenever the Y2 system does not start.
 */
/* PID1's standard output and error from now on: the screen (/dev/tty0) or the serial console (/dev/ttyS0). */
static void console_to(const char *path)
{
    long fd = call5(SYS_OPEN, (long)path, O_WRONLY | O_NOCTTY, 0, 0, 0);
    if (fd < 0) return;
    call5(SYS_DUP2, fd, 1, 0, 0, 0);
    call5(SYS_DUP2, fd, 2, 0, 0, 0);
    if (fd > 2) call5(SYS_CLOSE, fd, 0, 0, 0, 0);
}

/* The eMMC present (and read-only) before anything uses it. */
static void quiet_wait_disk(long watchdog)
{
    long emmc = wait_for_disk("/dev/mmcblk0", 100, watchdog);
    if (emmc >= 0) call5(SYS_CLOSE, emmc, 0, 0, 0, 0);
}
