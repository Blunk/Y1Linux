/* SPDX-License-Identifier: GPL-2.0-only */
/* Innioasis Y1 system sleep (docs/planning/Y1-POWER-PLAN.md §18 D7b): where the sleep journal lives. The Y2 keeps
 * it in stock's RAM-console SRAM; the Y1's stock kernel is not available to find the MT6572 equivalent, so the
 * Y1 reserves the last page of its second DRAM bank (innioasis-y1.dts, no-map). It survives the sleep (DRAM self
 * refresh); whether the preloader keeps it across a watchdog reset is unproven (y2_pm/retention tells, after a
 * self-test and a warm reboot). The resume vector writes its stamp here with the MMU off. */
#ifndef Y1_SLEEP_H
#define Y1_SLEEP_H
#define Y1_PM_JOURNAL_PHYS 0x9fbff000
#define Y1_PM_JOURNAL_STAMP (Y1_PM_JOURNAL_PHYS + 0x100)	/* Y2_PM_STAMP */
#endif
