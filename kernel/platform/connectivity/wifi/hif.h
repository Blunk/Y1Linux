/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Y2_WIFI_HIF_H
#define Y2_WIFI_HIF_H
/* The connectivity owner's header: the Y2's (../conn.h, -I$(src)/..) or, in the Innioasis Y1
 * build, the Y1's (Makefile). */
#include "conn.h"
struct y2_wifi;
typedef struct _GL_HIF_INFO_T {
    struct device *Dev;
    struct y2_wifi *bus;
    u32 ChipID;
    BOOLEAN fgIntReadClear, fgMbxReadClear, fgDmaEnable;
    void __iomem *HifRegBaseAddr;
#ifdef Y2_WIFI_PASSIVE_SCAN
    BOOLEAN fgScanPassive; /* the pending user scan listens only (Y2_WIFI_SCAN_*) */
#endif
} GL_HIF_INFO_T, *P_GL_HIF_INFO_T;
/*
 * The Innioasis Y1's passive scan (docs/planning/Y1-WIFI-PLAN.md W4; the Wi-Fi Makefile defines Y2_WIFI_PASSIVE_SCAN
 * only for the Y1). The vendor station scan is always active (ais_fsm.c); cfg80211's rule is that a request without
 * SSIDs listens only. gl_cfg80211.c records it per request, ais_fsm.c applies it to the user's own scan (never a
 * join's) and clears it. Both hooks sit at the end of an existing vendor line: those files keep every line number,
 * so their __LINE__ constants, and the Y2's objects, stay unchanged.
 */
#ifdef Y2_WIFI_PASSIVE_SCAN
#define Y2_WIFI_SCAN_REQUEST(glue, request) ((glue)->rHifInfo.fgScanPassive = (request)->n_ssids == 0)
#define Y2_WIFI_SCAN_TYPE(adapter, ais, msg) do { \
	if ((adapter)->prGlueInfo->rHifInfo.fgScanPassive && \
	    ((ais)->eCurrentState == AIS_STATE_SCAN || (ais)->eCurrentState == AIS_STATE_ONLINE_SCAN)) \
		(msg)->eScanType = SCAN_TYPE_PASSIVE_SCAN; \
	(adapter)->prGlueInfo->rHifInfo.fgScanPassive = FALSE; \
} while (0)
#else
#define Y2_WIFI_SCAN_REQUEST(glue, request) ((void)0)
#define Y2_WIFI_SCAN_TYPE(adapter, ais, msg) ((void)0)
#endif
#define CONF_HIF_LOOPBACK_AUTO 0
#define MTK_CHIP_ID_6582 0x6582
VOID HifRegDump(P_ADAPTER_T adapter);
BOOLEAN HifIsFwOwn(P_ADAPTER_T adapter);
WLAN_STATUS glRegisterBus(probe_card probe, remove_card remove);
VOID glUnregisterBus(remove_card remove);
VOID glResetHif(GLUE_INFO_T *glue);
VOID glSetHifInfo(P_GLUE_INFO_T glue, UINT_32 cookie);
VOID glClearHifInfo(P_GLUE_INFO_T glue);
VOID glGetChipInfo(GLUE_INFO_T *glue, UINT_8 *buf);
BOOL glBusInit(PVOID data);
VOID glBusRelease(PVOID data);
INT_32 glBusSetIrq(PVOID dev, PVOID isr, PVOID cookie);
VOID glBusFreeIrq(PVOID dev, PVOID cookie);
VOID glSetPowerState(P_GLUE_INFO_T glue, UINT_32 mode);
const unsigned char *y2_wifi_factory(P_GLUE_INFO_T glue);
void y2_wifi_error(P_GLUE_INFO_T glue);
#endif
