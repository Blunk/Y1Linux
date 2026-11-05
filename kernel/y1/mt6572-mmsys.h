/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6572 MMSYS routing for the display path OVL0 -> RDMA0 -> COLOR0 -> BLS -> DSI0 (Innioasis Y1,
 * docs/planning/Y1-DISPLAY-PLAN.md). Replaces the mt6572-mainline fork's table (603809c4357e, marked
 * guesswork) with the values the stock 3.0.2 kernel's disp_path_config_ writes for DSI video mode and
 * that LK leaves on the owner's player (display stage A): DISP_OVL_MOUT_EN 0x030 = 1, DISP_OUT_SEL
 * 0x04c = 0, RDMA0_OUT_SEL 0x050 = 0, COLOR_SEL 0x054 = 0, DSI_SEL 0x058 = 0. COLOR0 -> BLS has no
 * select. Field widths beyond the stock values are not known; the masks cover only what stock writes.
 */
#ifndef __SOC_MEDIATEK_MT6572_MMSYS_H
#define __SOC_MEDIATEK_MT6572_MMSYS_H

static const struct mtk_mmsys_routes mt6572_mmsys_routing_table[] = {
	MMSYS_ROUTE(OVL0, RDMA0, 0x030, BIT(0), BIT(0)),
	MMSYS_ROUTE(RDMA0, COLOR0, 0x050, BIT(0), 0),
	MMSYS_ROUTE(RDMA0, COLOR0, 0x054, BIT(0), 0),
	MMSYS_ROUTE(BLS, DSI0, 0x04c, 0x3, 0),
	MMSYS_ROUTE(BLS, DSI0, 0x058, BIT(0), 0),
};

#endif /* __SOC_MEDIATEK_MT6572_MMSYS_H */
