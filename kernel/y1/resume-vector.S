/* SPDX-License-Identifier: GPL-2.0-only */
/* Innioasis Y1 system sleep (docs/planning/Y1-POWER-PLAN.md §18 D7b): the boot ROM's entry after the suspend
 * firmware powered CPU0 down, in ARM state with the MMU and caches off. As the Y2's kernel/platform/resume-vector.S:
 * no stack and no Linux data, only the journal's stamp (Y2_PM_RESUME_STAMP) at its physical address, which proves
 * the reset path to the sleep helper, then Linux's cpu_resume. */
#include <linux/linkage.h>
#include "sleep.h"
 .syntax unified
 .arm
 .text
 .align 5
ENTRY(y1_cpu_resume)
 movw r0, #(Y1_PM_JOURNAL_STAMP & 0xffff)
 movt r0, #(Y1_PM_JOURNAL_STAMP >> 16)
 movw r1, #0x5253
 movt r1, #0x5932
 str r1, [r0]
 dsb sy
 b cpu_resume_arm
ENDPROC(y1_cpu_resume)
