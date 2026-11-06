/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Y2_CONN_H
#define Y2_CONN_H
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include "protocol.h"

#define Y2_CONN_BT 0
#define Y2_CONN_FM 1
#define Y2_CONN_WIFI 3
#define Y2_CONN_WMT 4
#define Y2_CONN_DMA_SIZE 8192
#define Y2_CONN_FW "mediatek/mt6582/"
/* The owner's stock firmware inputs (firmware.c), pinned by size and SHA-256. */
#define Y2_CONN_FIRMWARE { \
	{Y2_CONN_FW "WIFI_RAM_CODE_MT6582", 160368, {0x4d,0xef,0x37,0x52,0x2e,0x2b,0xfd,0x2d,0x4b,0x78,0xe0,0x1b,0xf9,0xf5,0x67,0xa3,0x7e,0x3f,0x67,0xab,0x3d,0x7b,0x66,0x46,0xff,0xb3,0x17,0xf5,0xb2,0xb4,0x9b,0x82}}, \
	{Y2_CONN_FW "WMT_SOC.cfg", 80, {0xce,0xa0,0xa9,0x4b,0x8a,0x88,0xe9,0xc2,0x4e,0x68,0x35,0x44,0x6e,0xbc,0x31,0x06,0xd7,0xb1,0x00,0xc3,0x70,0x9e,0xa9,0x8e,0x8b,0xb7,0x36,0x86,0x89,0xc8,0xa2,0xb5}}, \
	{Y2_CONN_FW "mt6572_82_patch_e1_0_hdr.bin", 33120, {0xfa,0x3b,0xe4,0xa9,0x25,0x40,0x29,0x6d,0xe5,0x12,0xd6,0x4f,0x56,0x27,0x27,0x46,0xbe,0x10,0xbd,0xfa,0x2d,0xb5,0x71,0xc8,0xce,0xad,0xa2,0xd4,0x0c,0xd6,0x16,0xf0}}, \
	{Y2_CONN_FW "mt6572_82_patch_e1_1_hdr.bin", 17476, {0x96,0xeb,0x25,0x48,0x93,0x05,0xbd,0x47,0x28,0x05,0xdb,0x1d,0xa3,0x3b,0xcc,0x0a,0xcd,0x37,0xb5,0xbb,0x21,0xcc,0xed,0xa9,0x8d,0x7e,0x88,0xba,0x11,0x79,0xca,0x37}}, \
	{Y2_CONN_FW "modem_1_2g_n.img", 2424376, {0x50,0x59,0x77,0x59,0x75,0xcb,0xf6,0xab,0x74,0xc4,0x39,0x78,0xca,0x8f,0x65,0xd9,0xa2,0x74,0xb8,0x35,0x85,0xf4,0x56,0x13,0x4e,0x39,0xb0,0x9f,0x6d,0xc7,0xa4,0xf1}}, \
	{Y2_CONN_FW "mt6627_fm_v1_patch.bin", 1028, {0xba,0x35,0xc7,0x4d,0x26,0x66,0x07,0x29,0xca,0xf8,0x3b,0x41,0xa5,0x67,0xe7,0x00,0xf6,0x33,0xb6,0xc9,0x93,0x84,0xdf,0xa6,0xcb,0xb3,0xc1,0x7c,0xeb,0x76,0x7e,0x26}}, \
	{Y2_CONN_FW "mt6627_fm_v1_coeff.bin", 1092, {0xd3,0x2f,0x4a,0x16,0xb3,0x5f,0x95,0x0e,0xd1,0xa5,0x7f,0x2a,0xe8,0x8f,0x77,0xac,0x4b,0x56,0x74,0xd5,0xc4,0x21,0x0b,0x65,0xf6,0xfe,0x79,0x12,0x90,0x1f,0xd8,0xdb}}, \
}
/* The chip whose HVR 8a01/FVR 8a00 controller needs HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2 (hci.c): the Y2. */
#define Y2_CONN_EXT_FEATURES_QUIRK_CHIP 0x6582
/* Hash of the generic, non-unique MT6582 address in BOTH this Y2's stock
 * libcustom_nvram and libbluetooth_mtk (hci.c's reject value). */
#define Y2_CONN_DEFAULT_ADDRESS_SHA256 { \
	0x8b,0xf9,0xd1,0xe7,0x4e,0xea,0x89,0xe5,0x1c,0xfb,0x4d,0x83,0x22,0x59,0x02,0x12, \
	0xd2,0x7b,0xde,0x6e,0xe8,0x4f,0x49,0x7f,0xb7,0xaf,0x57,0xdf,0xf1,0xa9,0xd9,0xf6, \
}
struct firmware;
int y2_conn_request_firmware(struct device *dev, const char *name, const struct firmware **fw);
struct hci_dev;
struct y2_md;
struct y2_fm;
struct y2_stp_frame { unsigned size; unsigned char data[Y2_STP_MAX_PAYLOAD + 6]; };
struct y2_conn {
	struct device *dev;
	struct mutex lifecycle, command, stp_lock, dma_tx, dma_rx, recovery_lock;
	struct clk_bulk_data clocks[3];
	struct regulator_bulk_data rails[4];
	struct regmap *pmic;
	struct reset_control *reset;
	bool rail_on[4], clock_on, factory_ready, activated, recovering, pmic_ready;
	unsigned long last_recovery;
	unsigned automatic_recoveries;
	bool hci_recovery_pending, wifi_wanted, md_owned;
	void __iomem *mcu, *top, *emi, *btif, *txdma, *rxdma;
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
	unsigned char wifi_factory[512], bt_factory[64];
	struct platform_device *wifi;
	struct y2_md *md;
	struct y2_fm *fm;
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
int y2_wmt_boot(struct y2_conn *c);
int y2_wmt_function(struct y2_conn *c, unsigned function, bool enable);
int y2_conn_rail(struct y2_conn *c, unsigned rail, bool enable);
int y2_hci_register(struct y2_conn *c);
void y2_hci_unregister(struct y2_conn *c);
void y2_hci_receive(struct y2_conn *c, const unsigned char *data, unsigned size);
int y2_md_probe(struct platform_device *pdev, struct y2_conn *c);
int y2_md_calibrate(struct y2_conn *c);
void y2_md_unregister(struct y2_conn *c);
void y2_md_abort(struct y2_conn *c);
void y2_conn_hci_error(struct y2_conn *c);
int y2_fm_probe(struct platform_device *pdev, struct y2_conn *c);
void y2_fm_receive(struct y2_conn *c, const unsigned char *data, unsigned size);
void y2_fm_lost(struct y2_conn *c);
void y2_fm_shutdown(struct y2_conn *c);
#endif
