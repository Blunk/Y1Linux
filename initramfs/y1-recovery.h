/* SPDX-License-Identifier: GPL-2.0-only
 * Innioasis Y1 PID1 in the RECOVERY image (docs/planning/Y1-RECOVERY-PLAN.md, R1 probe): the recovery tree
 * adds y2.recovery=1 to the command line, the only difference to BOOTIMG. The probe proves that LK started
 * RECOVERY: it never hands over to the system, shows "Recovery works" and keeps PID1's USB shell (cat, poweroff).
 */

/* word is one whole space-separated word of line. */
static int has_word(const char *line, const char *word)
{
    while (*line) {
        while (*line == ' ' || *line == '\n') ++line;
        const char *a = line, *b = word;
        while (*b && *a == *b) { ++a; ++b; }
        if (!*b && (!*a || *a == ' ' || *a == '\n')) return 1;
        while (*line && *line != ' ' && *line != '\n') ++line;
    }
    return 0;
}

static int recovery_mode(void)
{
    char cmdline[512];
    return read_text("/proc/cmdline", cmdline, sizeof(cmdline)) > 0 && has_word(cmdline, "y2.recovery=1");
}

static void recovery_probe(void)
{
    console_to("/dev/tty0");
    say("\n\n  Recovery works\n\n  Y1 recovery probe: LK started RECOVERY.\n"
        "  The system is not started; PID1's shell is on USB.\n");
    say("y1-init: recovery probe (y2.recovery=1), no root hand-over\n");
}

static int recovery_selftest(void)
{
    return has_word("rdinit=/init loglevel=3 y2.recovery=1", "y2.recovery=1") &&
           has_word("y2.recovery=1 panic=10\n", "y2.recovery=1") &&
           !has_word("rdinit=/init y2.recovery=10", "y2.recovery=1") &&
           !has_word("xy2.recovery=1", "y2.recovery=1") && !has_word("", "y2.recovery=1");
}
