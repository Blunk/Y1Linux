/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Innioasis Y1: the eMMC write guard (docs/planning/Y1-DATA-PLAN.md, stage B2). Linux sees the Y1's eMMC as one
 * disk without partitions; Y2DATA is a loop over USRDATA, so the disk has to be writable. The guard checks every
 * request to the eMMC host before it reaches the card: writes only inside one window of the user area (USRDATA,
 * from the device tree), no write to the boot or RPMB partitions, no erase, write protect, lock or CID/CSD
 * programming, and EXT_CSD switches only to the volatile settings Linux uses at runtime. Reads are not limited.
 * After the Y2's storage-policy.h, without its MT6582 modes and tuning.
 */
#ifndef Y1_EMMC_GUARD_H
#define Y1_EMMC_GUARD_H

struct y1_emmc_request {
	unsigned opcode, arg;
	unsigned write, blocks, blksz;			/* data phase: write 1 = to the card */
	unsigned has_data;
	unsigned has_sbc, sbc_arg;			/* CMD23 before the data command */
	unsigned partition;				/* PARTITION_ACCESS now in effect (0 = user) */
	unsigned part_config;				/* the card's PARTITION_CONFIG (boot bits) */
	unsigned blockaddr;				/* sector addressing (high capacity) */
	unsigned window_start, window_sectors;		/* the writable user-area window */
};

/* The writable window, in 512-byte sectors of the user area; overflow-safe. */
static inline int y1_emmc_write_inside(const struct y1_emmc_request *r)
{
	if (!r->blocks || r->arg < r->window_start)
		return 0;
	return r->arg - r->window_start <= r->window_sectors &&
	       r->blocks <= r->window_sectors - (r->arg - r->window_start);
}

/* CMD6 (SWITCH) with access "write byte" to an EXT_CSD field Linux sets at runtime. */
static inline int y1_emmc_switch_allowed(const struct y1_emmc_request *r)
{
	unsigned index = (r->arg >> 16) & 0xff, value = (r->arg >> 8) & 0xff;

	if ((r->arg & 0xff0000ffU) != 0x03000001U && (r->arg & 0xff0000ffU) != 0x03000000U)
		return 0;
	switch (index) {
	case 32: return value == 1;			/* FLUSH_CACHE */
	case 33: return value <= 1;			/* CACHE_CTRL */
	case 34: return value >= 1 && value <= 3;	/* POWER_OFF_NOTIFICATION */
	case 161: return value <= 1;			/* HPI_MGMT */
	case 175: return value <= 1;			/* ERASE_GROUP_DEF */
	case 179: return (value & ~7U) == (r->part_config & ~7U);	/* access bits only */
	case 183: return value <= 2 || value == 5 || value == 6;	/* BUS_WIDTH */
	case 185: return value <= 2;			/* HS_TIMING: legacy, HS, HS200 */
	case 187: return 1;				/* POWER_CLASS (volatile) */
	default: return 0;				/* BOOT_WP, RST_n, BKOPS, PARTITION_SETTING, ... */
	}
}

static inline int y1_emmc_request_allowed(const struct y1_emmc_request *r)
{
	if (r->has_data && r->write) {
		if ((r->opcode != 24 && r->opcode != 25) || (r->opcode == 24 && r->blocks != 1))
			return 0;
		if (r->partition || !r->blockaddr || r->blksz != 512 || !y1_emmc_write_inside(r))
			return 0;
		/* CMD23: the block count, optionally reliable write (bit 31) and the data tag (bit 29). */
		return !r->has_sbc || (r->sbc_arg & ~0xa0000000U) == r->blocks;
	}
	if (r->has_data)				/* reads: data, EXT_CSD, tuning */
		return r->opcode == 17 || r->opcode == 18 || r->opcode == 8 || r->opcode == 21;
	switch (r->opcode) {
	case 0: return r->arg == 0;			/* not boot or pre-idle */
	case 1: case 2: case 3: case 5: case 7: case 9: case 10: case 12: case 13: case 23:
		return 1;				/* identification, select, sleep, stop, status, count */
	case 6: return y1_emmc_switch_allowed(r);
	case 16: return r->arg == 512;			/* SET_BLOCKLEN */
	default: return 0;				/* erase/trim, WP, lock, program CID/CSD, gen cmd */
	}
}

#endif
