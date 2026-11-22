// SPDX-License-Identifier: GPL-2.0-only
/* The Y2's sleep journal (kernel/platform/pm-journal.c, added unchanged as y2-pm-journal.c) on the Y1's reserved
 * DRAM page (sleep.h): the stage record, the device-callback ring, the RGU backstop and /sys/firmware/y2_pm,
 * which the shared sleep helper reads (docs/planning/Y1-POWER-PLAN.md §18 D7b). */
#include "sleep.h"
#define Y2_PM_PHYS Y1_PM_JOURNAL_PHYS
#define Y2_PM_MACHINE "innioasis,y1"
#define Y2_PM_RETENTION "reserved_dram_unproven"
#include "y2-pm-journal.c"
