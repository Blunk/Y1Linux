/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Innioasis Y1: the MT6323 audio registers the headphone path may write, and the values
 * (docs/planning/Y1-AUDIO-PLAN.md §8: the stock HAL's power-up and power-down). Applied by the Y1 PMIC
 * wrapper's write policy next to the Y2's charger and ISINK fields (policy.h). Each guard admits only the
 * stock sequence's values at 44.1 kHz with a zero DC trim; any other write is refused.
 */
#ifndef Y1_PMIC_AUDIO_POLICY_H
#define Y1_PMIC_AUDIO_POLICY_H

static inline int y1_pmic_audio_register(unsigned int reg)
{
	switch (reg) {
	case 0x10a: case 0x10c:
	case 0x4000: case 0x4002: case 0x4006: case 0x4008: case 0x4014: case 0x4016: case 0x4024:
	case 0x700: case 0x708: case 0x70a: case 0x70c:
		return 1;
	}
	return 0;
}

/* value: the full new value; old: the register's current value. */
static inline int y1_pmic_audio_allowed(unsigned int reg, unsigned int value, unsigned int old)
{
	unsigned int changed = old ^ value;

	switch (reg) {
	case 0x10a: case 0x10c:		/* TOP_CKPDN1 set/clear: the audio clock bit only */
		return value == 0x100;
	case 0x4000: case 0x4014:	/* enable bits */
		return !(changed & ~1U);
	case 0x4002:			/* the 44.1 kHz rate code */
		return !(changed & ~0xfU) && (value & 0xf) == 9;
	case 0x4006: case 0x4008:	/* DC trim: zero */
		return value == 0;
	case 0x4016:			/* the handshake toggle */
		return !(changed & ~0x300U);
	case 0x4024:			/* NEWIF at 44.1 kHz */
		return value == 0x7330;
	case 0x700:			/* bits 15:12: 7 on, 6 off */
		return !(changed & ~0xf000U) && ((value >> 12) == 7 || (value >> 12) == 6);
	case 0x708:
		return value == 0 || value == 0x007c;
	case 0x70a:			/* 0x0014 plus the two gain fields */
		return (value & ~0x7700U) == 0x0014;
	case 0x70c:
		return value == 0xf7f2 || value == 0xf5ba || value == 0x37e2;
	}
	return 0;
}

#endif
