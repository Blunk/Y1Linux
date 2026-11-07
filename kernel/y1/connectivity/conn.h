/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Y2_CONN_H
#define Y2_CONN_H
/*
 * Innioasis Y1 (MT6572) build of the Y2's connectivity owner (docs/planning/Y1-BLUETOOTH-PLAN.md
 * B2). The Y2's btif.c, stp.c, hci.c, firmware.c and protocol.h are compiled unchanged against this
 * header (it keeps the Y2's names and include guard), so it carries every field they use; the
 * Y1's own core.c and wmt.c hold what differs. Bluetooth only: no Wi-Fi, no modem, no RGU reset.
 */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
/* The MT6572's calibration-event rule for the shared stp.c (y1-conn-policy.h). */
#define Y2_CONN_RF_RESULT(c, p, size) y1_conn_rf_result(p, size)
#include "protocol.h"
#include "y1-conn-policy.h"

#define Y2_CONN_BT 0
#define Y2_CONN_WMT 4
#define Y2_CONN_DMA_SIZE 8192
/* firmware_class.path=/data/firmware: the platform installs the owner's copy in /data/firmware/mt6572
 * (y2-platform firmware, the Y1 table; Y1-BLUETOOTH-PLAN.md §16). */
#define Y2_CONN_FW "mt6572/"
/* The owner's stock /system/etc/firmware (docs/knowledge/evidence/y1-firmware-inventory.json). */
#define Y2_CONN_FIRMWARE { \
	{Y2_CONN_FW "WMT_SOC.cfg", 82, {0x16,0x1f,0x18,0xb6,0x86,0xe5,0x9a,0x76,0x33,0x68,0xff,0xb1,0xd8,0x22,0x9f,0x15,0x8f,0xa6,0x14,0x93,0xec,0xc3,0xdf,0xf5,0xc6,0xef,0x8c,0x11,0xa7,0xf5,0xf1,0xe0}}, \
	{Y2_CONN_FW "mt6572_82_patch_e1_0_hdr.bin", 39868, {0x5c,0x13,0xeb,0xad,0x70,0xfb,0xdd,0xc1,0x04,0x10,0xc6,0x75,0xfd,0xce,0x84,0xe8,0x9f,0x5d,0xb9,0xfa,0x13,0x23,0xf9,0xd7,0x87,0xec,0xa3,0xe9,0x6a,0x99,0xbf,0xc1}}, \
}
/* The chip whose HVR 8a01/FVR 8a00 controller needs HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2 (hci.c): the Y1 (B6: the same HVR 8a01/FVR 8a00 firmware rejects page 2 with status 0x30, seen as -38). */
#define Y2_CONN_EXT_FEATURES_QUIRK_CHIP 0x6572
#define Y2_CONN_DEFAULT_ADDRESS_SHA256 Y1_CONN_DEFAULT_ADDRESS_SHA256
struct firmware;
int y2_conn_request_firmware(struct device *dev, const char *name, const struct firmware **fw);
struct hci_dev;
struct y2_stp_frame { unsigned size; unsigned char data[Y2_STP_MAX_PAYLOAD + 6]; };
struct y2_conn {
	struct device *dev;
	struct mutex lifecycle, command, stp_lock, dma_tx, dma_rx, recovery_lock;
	/* spm, btif, dma: btif.c owns [1] and [2]; the SPM clock is never gated again (stock). */
	struct clk_bulk_data clocks[3];
	/* vcn18, vcn28, vcn33-bt, vcn33-wifi (the last only for RF calibration, as stock). */
	struct regulator_bulk_data rails[4];
	struct regmap *pmic, *spm, *infracfg;
	bool rail_on[4], clock_on, factory_ready, activated, recovering, pmic_ready;
	unsigned long last_recovery;
	unsigned automatic_recoveries;
	bool hci_recovery_pending;
	void __iomem *mcu, *emi, *btif, *txdma, *rxdma;
	int irq[4];
	unsigned char *txbuf, *rxbuf;
	dma_addr_t txaddr, rxaddr;
	unsigned txptr, rxptr;
	bool transport_on, dma_active, full_stp, powered, suspended, removing, calibrated;
	unsigned functions, wanted, hvr, fvr, chip;
	unsigned stp_tx, stp_oldest, stp_rx, stp_pending, rx_used, rx_needed;
	unsigned stp_retries, transport_errors, recoveries;
	int failure;
	unsigned char rx_frame[Y2_STP_MAX_PAYLOAD + 6];
	struct y2_stp_frame window[8];
	wait_queue_head_t tx_wait;
	struct delayed_work retry_work;
	struct work_struct recovery_work;
	struct completion response;
	unsigned response_size;
	bool wmt_reg_read, wmt_rf_calibrate;
	unsigned char response_data[256];
	struct hci_dev *hdev;
	struct sk_buff *hci_rx;
	struct sk_buff_head hci_tx;
	struct work_struct hci_work;
	bool hci_open, hci_paused;
	bool bt_identity_set, bt_controller_address;
	unsigned char bt_address[6];
	unsigned hci_needed;
	unsigned char bt_factory[Y1_CONN_RECORD_SIZE];
	/* Bring-up stages (B4/B5): the DT mode, a pending abort, the calibration event seen. */
	unsigned mode, calibration_size, calibration_status;
	bool abort;
};
int y2_btif_probe(struct platform_device *pdev, struct y2_conn *c);
int y2_btif_start(struct y2_conn *c);
int y2_btif_stop(struct y2_conn *c);
int y2_btif_abort(struct y2_conn *c);
int y2_btif_send(struct y2_conn *c, const unsigned char *data, unsigned size);
void y2_btif_report_timeout(struct y2_conn *c);
void y2_stp_init(struct y2_conn *c);
void y2_stp_reset(struct y2_conn *c);
void y2_stp_receive(struct y2_conn *c, const unsigned char *data, unsigned size);
int y2_stp_send(struct y2_conn *c, unsigned channel, const unsigned char *data, unsigned size);
void y2_conn_failed(struct y2_conn *c, int error);
int y2_conn_get(struct y2_conn *c, unsigned function);
int y2_conn_put(struct y2_conn *c, unsigned function);
int y2_conn_wmt(struct y2_conn *c, const unsigned char *request, unsigned size,
	       const unsigned char *expected, unsigned expected_size);
int y1_wmt_boot(struct y2_conn *c);
int y1_wmt_sleep(struct y2_conn *c);
int y2_wmt_function(struct y2_conn *c, unsigned function, bool enable);
int y2_conn_rail(struct y2_conn *c, unsigned rail, bool enable);
int y2_hci_register(struct y2_conn *c);
void y2_hci_unregister(struct y2_conn *c);
void y2_hci_receive(struct y2_conn *c, const unsigned char *data, unsigned size);
void y2_conn_hci_error(struct y2_conn *c);
#endif
