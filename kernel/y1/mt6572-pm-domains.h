/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Innioasis Y1: the MT6572's GPU power domain for mainline's mtk-pm-domains (docs/planning/Y1-REBORN-PLAN.md,
 * stage R1b). From the mt6572-mainline fork's mt6572-pm-domains.h with the display domain left out: LK powers
 * the display and Linux must not switch it off, so only MFG is described (control 0x10006214, status bit 4,
 * SRAM bit 8 and its acknowledge bit 12, no bus protection; the same registers as stock 3.0.2's SYS_MFG).
 */
#ifndef __SOC_MEDIATEK_MT6572_PM_DOMAINS_H
#define __SOC_MEDIATEK_MT6572_PM_DOMAINS_H

#include "mtk-pm-domains.h"
#include <dt-bindings/power/mediatek,mt6572-power-controller.h>

static const struct scpsys_domain_data scpsys_domain_data_mt6572[] = {
	[MT6572_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(12),
	},
};

static const struct scpsys_soc_data mt6572_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6572,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6572),
};

#endif /* __SOC_MEDIATEK_MT6572_PM_DOMAINS_H */
