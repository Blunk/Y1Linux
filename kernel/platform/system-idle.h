/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Y2_SYSTEM_IDLE_H
#define Y2_SYSTEM_IDLE_H
#include <linux/kconfig.h>
/* The Y2's coordinator (CONFIG_Y2_POWER) or the Y1's port of it (CONFIG_INNIOASIS_Y1_IDLE, kernel/y1/system-idle.c). */
#if IS_ENABLED(CONFIG_Y2_POWER) || IS_ENABLED(CONFIG_INNIOASIS_Y1_IDLE)
void y2_system_idle_activity(void);
int y2_system_idle_restore(void);
bool y2_backlight_dark(void);
bool y2_workload_active(void);
bool y2_workload_idle_blocked(void);
#else
static inline void y2_system_idle_activity(void)
{
}
static inline int y2_system_idle_restore(void)
{
	return 0;
}
#endif
/* Y1 AFE stream lease (bit 0 playback, bit 1 FM/capture); callable from atomic context. */
#if IS_ENABLED(CONFIG_INNIOASIS_Y1_IDLE)
void y1_system_idle_lease(unsigned bit, bool on);
bool y1_system_idle_quiet(void);
#else
static inline void y1_system_idle_lease(unsigned bit, bool on)
{
}
#endif
#endif
