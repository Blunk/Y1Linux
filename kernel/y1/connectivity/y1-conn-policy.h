/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Y1_CONN_POLICY_H
#define Y1_CONN_POLICY_H
/*
 * Innioasis Y1 (MT6572) connectivity facts as data, shared with the host tests
 * (docs/planning/Y1-BLUETOOTH-PLAN.md B1/B2; every value from the stock decode in
 * out/y1-bluetooth-b1-notes.md, stock addresses there). Pure C: no kernel headers.
 */

#define Y1_CONN_CHIP 0x6572
#define Y1_CONN_EMI_START 0x80000000u
#define Y1_CONN_EMI_SIZE 0x100000u

/* The fixed wiring: a node with any other window or interrupt is rejected before any
 * mapping, DMA or power. The radio core's memory is fixed at 0x80000000 (no remap on
 * this chip); 0x1100c000 is the SPI controller here, not BTIF as on the Y2. */
struct y1_conn_range { const char *name; unsigned start, size; };
static const struct y1_conn_range y1_conn_ranges[] = {
	{"mcu", 0x18070000, 0x1000}, {"conn-emi", Y1_CONN_EMI_START, Y1_CONN_EMI_SIZE},
	{"btif", 0x1100e000, 0x100}, {"tx-dma", 0x11000380, 0x80}, {"rx-dma", 0x11000400, 0x80},
};
/* GIC SPI numbers (stock GIC 84/73/74/154 less 32); level-low: the wake line CONFIRMED
 * (stock IRQF_TRIGGER_LOW), BTIF and the DMA channels INFERRED as on the Y2. */
#define Y1_CONN_GIC_SPI 0
#define Y1_CONN_LEVEL_LOW 8
struct y1_conn_irq { const char *name; unsigned spi; };
static const struct y1_conn_irq y1_conn_irqs[] = {
	{"btif", 52}, {"tx-dma", 41}, {"rx-dma", 42}, {"wake", 122},
};

/*
 * The CONN power domain (SPM 0x10006000, INFRACFG 0x10001000), stock
 * spm_mtcmos_ctrl_general_locked for subsystem CONN. Kept in this driver rather than in
 * mtk-pm-domains: mainline's generic sequence cannot express stock's clock-isolation pulse
 * around RST_B (CLK_DIS set again while the reset is released, then cleared) nor its
 * 1/3 us settle times, and the MT6572 binding has no CONN index. Bit names are mainline's
 * (mtk-pm-domains.c); the B1 notes call bit 4 "ISO" and bit 1 "CLK_DIS", same values.
 */
#define Y1_SPM_KEY 0x0b160001u
#define Y1_SPM_CONN_PWR_CON 0x280
#define Y1_SPM_PWR_STATUS 0x60c
#define Y1_SPM_PWR_STATUS_2ND 0x610
#define Y1_SPM_CONN_STATUS 0x2
#define Y1_SPM_SLEEP_STATUS 0x3b4 /* bit 11 set: CONN awake (stock spm_is_conn_sleep) */
#define Y1_SPM_CONN_AWAKE (1u << 11)
#define Y1_INFRA_BUS_PROT 0x220
#define Y1_INFRA_BUS_PROT_STA 0x228
#define Y1_INFRA_CONN_PROT 0x11
#define Y1_PWR_RST_B 0x1
#define Y1_PWR_ISO 0x2
#define Y1_PWR_ON 0x4
#define Y1_PWR_ON_2ND 0x8
#define Y1_PWR_CLK_DIS 0x10

enum y1_conn_op {
	Y1_SPM_WRITE, Y1_SPM_SET, Y1_SPM_CLEAR, Y1_SPM_POLL_SET, Y1_SPM_POLL_CLEAR,
	Y1_INFRA_SET, Y1_INFRA_CLEAR, Y1_INFRA_POLL_SET, Y1_INFRA_POLL_CLEAR, Y1_DELAY_US,
};
struct y1_conn_step { unsigned char op; unsigned short reg; unsigned value; };

/* Stock conn_power_on: key; PWR_ON, 1 us; PWR_ON_2ND, 3 us; both acks; CLK_DIS and ISO off;
 * the clock pulse around RST_B; then the bus protection (mask 0x11, Y2 0x104) released. */
static const struct y1_conn_step y1_conn_domain_on[] = {
	{Y1_SPM_WRITE, 0, Y1_SPM_KEY},
	{Y1_SPM_SET, Y1_SPM_CONN_PWR_CON, Y1_PWR_ON}, {Y1_DELAY_US, 0, 1},
	{Y1_SPM_SET, Y1_SPM_CONN_PWR_CON, Y1_PWR_ON_2ND}, {Y1_DELAY_US, 0, 3},
	{Y1_SPM_POLL_SET, Y1_SPM_PWR_STATUS, Y1_SPM_CONN_STATUS},
	{Y1_SPM_POLL_SET, Y1_SPM_PWR_STATUS_2ND, Y1_SPM_CONN_STATUS},
	{Y1_SPM_CLEAR, Y1_SPM_CONN_PWR_CON, Y1_PWR_CLK_DIS},
	{Y1_SPM_CLEAR, Y1_SPM_CONN_PWR_CON, Y1_PWR_ISO},
	{Y1_SPM_SET, Y1_SPM_CONN_PWR_CON, Y1_PWR_CLK_DIS},
	{Y1_SPM_SET, Y1_SPM_CONN_PWR_CON, Y1_PWR_RST_B},
	{Y1_SPM_CLEAR, Y1_SPM_CONN_PWR_CON, Y1_PWR_CLK_DIS},
	{Y1_INFRA_CLEAR, Y1_INFRA_BUS_PROT, Y1_INFRA_CONN_PROT},
	{Y1_INFRA_POLL_CLEAR, Y1_INFRA_BUS_PROT_STA, Y1_INFRA_CONN_PROT},
};
/* Stock conn_power_off after the sleep wait: bus protection first, then isolate, reset,
 * both power switches with 1 us each, and both acks cleared. */
static const struct y1_conn_step y1_conn_domain_off[] = {
	{Y1_SPM_WRITE, 0, Y1_SPM_KEY},
	{Y1_INFRA_SET, Y1_INFRA_BUS_PROT, Y1_INFRA_CONN_PROT},
	{Y1_INFRA_POLL_SET, Y1_INFRA_BUS_PROT_STA, Y1_INFRA_CONN_PROT},
	{Y1_SPM_SET, Y1_SPM_CONN_PWR_CON, Y1_PWR_CLK_DIS},
	{Y1_SPM_SET, Y1_SPM_CONN_PWR_CON, Y1_PWR_ISO},
	{Y1_SPM_CLEAR, Y1_SPM_CONN_PWR_CON, Y1_PWR_RST_B},
	{Y1_SPM_CLEAR, Y1_SPM_CONN_PWR_CON, Y1_PWR_ON}, {Y1_DELAY_US, 0, 1},
	{Y1_SPM_CLEAR, Y1_SPM_CONN_PWR_CON, Y1_PWR_ON_2ND}, {Y1_DELAY_US, 0, 1},
	{Y1_SPM_POLL_CLEAR, Y1_SPM_PWR_STATUS, Y1_SPM_CONN_STATUS},
	{Y1_SPM_POLL_CLEAR, Y1_SPM_PWR_STATUS_2ND, Y1_SPM_CONN_STATUS},
};

/*
 * The power-on as named stages, run in order by core.c (stock mtk_wcn_consys_hw_pwr_on, B1 §1.1): the
 * MCU window is read only after DOMAIN_ON has released RST_B; BTIF opens only after the chip ID. The
 * chip-id bring-up (B4, DT "innioasis,bring-up = \"chip-id\"") stops there and powers off: no PMIC
 * init subset, no BTIF or DMA, no firmware, no HCI. OFF = sleep wait, domain off, rails off.
 */
enum y1_conn_stage {
	Y1_STAGE_NORMALISE, Y1_STAGE_VCN18, Y1_STAGE_VCN28, Y1_STAGE_EMI_CLEAR, Y1_STAGE_SPM_CLOCK,
	Y1_STAGE_DOMAIN_ON, Y1_STAGE_CHIP_ID, Y1_STAGE_BTIF, Y1_STAGE_WMT, Y1_STAGE_OFF,
};
static const unsigned char y1_conn_full_stages[] = {
	Y1_STAGE_NORMALISE, Y1_STAGE_VCN18, Y1_STAGE_VCN28, Y1_STAGE_EMI_CLEAR, Y1_STAGE_SPM_CLOCK,
	Y1_STAGE_DOMAIN_ON, Y1_STAGE_CHIP_ID, Y1_STAGE_BTIF, Y1_STAGE_WMT,
};
static const unsigned char y1_conn_chip_id_stages[] = {
	Y1_STAGE_NORMALISE, Y1_STAGE_VCN18, Y1_STAGE_VCN28, Y1_STAGE_EMI_CLEAR, Y1_STAGE_SPM_CLOCK,
	Y1_STAGE_DOMAIN_ON, Y1_STAGE_CHIP_ID, Y1_STAGE_OFF,
};
/* MT6323: VCN_1V8 low-power bit (0x512 bit 1, cleared before the enable at bit 14) and VCN28's
 * hardware (SRCLKEN) mode (0x41c bit 14, set before the enable at bit 12: the TCXO path). */
#define Y1_PMIC_VCN18_CON 0x512
#define Y1_PMIC_VCN18_LP (1u << 1)
#define Y1_PMIC_VCN28_CON 0x41c
#define Y1_PMIC_VCN28_HW_MODE (1u << 14)

/* The Y1's WMT_SOC.cfg: exactly these lines, each once. No co_clock_flag: the Y1 runs
 * the TCXO path (VCN28 its own, no WMT co-clock command); sdio_driving_cfg is read by
 * stock but unused on BTIF. Returns the line's bit, or 0 for anything else. */
static const char *const y1_conn_config_lines[] = {
	"coex_wmt_ant_mode=1", "wmt_gps_lna_pin=0", "wmt_gps_lna_enable=0", "sdio_driving_cfg=0",
};
#define Y1_CONN_CONFIG_ALL ((1u << 4) - 1)
static inline unsigned y1_conn_config_line(const char *line)
{
	for (unsigned i = 0; i < sizeof(y1_conn_config_lines) / sizeof(y1_conn_config_lines[0]); i++) {
		const char *a = line, *b = y1_conn_config_lines[i];
		while (*a && *a == *b) { a++; b++; }
		if (!*a && !*b) return 1u << i;
	}
	return 0;
}

/* mt6572_82_patch_e1_0_hdr.bin: build time at 0, platform "1319" at 16, HW 8a 00 at 20,
 * FW at 22 (stock's launcher compares only byte 23 with the FVR's low byte), then byte 24
 * = count << 4 | sequence = 0x11 (one part) and the download address at 25-27. The payload
 * starts at 28. P_ADDRESS bytes 12-15 get the four info bytes with byte 0 forced to 0, as the
 * launcher: 00 00 06 00 for the owner's file. Returns 0 and fills address, or -1. */
#define Y1_CONN_PATCH_PAYLOAD 28
static inline int y1_conn_patch_header(const unsigned char *p, unsigned size, unsigned fvr,
				       unsigned char address[4])
{
	if (size <= Y1_CONN_PATCH_PAYLOAD || size > 65536 || p[16] != '1' || p[17] != '3' ||
	    p[18] != '1' || p[19] != '9' || p[20] != 0x8a || p[21] || p[23] != (fvr & 0xff) ||
	    p[24] != 0x11)
		return -1;
	address[0] = 0; address[1] = p[25]; address[2] = p[26]; address[3] = p[27];
	return 0;
}

/* The Y1's BT_Addr record: 32 bytes, the first 30 the Y2 record's layout (address 0-5 in
 * display order, radio 12-17, sleep 18-24, TX power offset 27-29; B1 §4). The address must
 * be unicast, non-zero and not the chip's generic default (stock's 00:00:46:65:72:01, kept
 * only as this hash: a reject value, never an identity). digest = SHA-256 of bytes 0-5. */
#define Y1_CONN_RECORD_SIZE 32
#define Y1_CONN_DEFAULT_ADDRESS_SHA256 { \
	0xd3,0x09,0x7b,0x82,0x86,0x4e,0x72,0x4d,0xb5,0xa8,0xf3,0xa0,0xd2,0x1a,0x5d,0xed, \
	0x90,0xa9,0x17,0x3d,0x2c,0x04,0x56,0x8f,0xb1,0x8c,0xbd,0x76,0xe2,0x7c,0x1b,0x85, \
}
static inline int y1_conn_record_check(const unsigned char *record, unsigned size,
				       const unsigned char digest[32])
{
	static const unsigned char generic[32] = Y1_CONN_DEFAULT_ADDRESS_SHA256;
	unsigned any = 0, same = 0;
	if (size != Y1_CONN_RECORD_SIZE || (record[0] & 1)) return -1;
	for (unsigned i = 0; i < 6; i++) any |= record[i];
	for (unsigned i = 0; i < 32; i++) same |= digest[i] ^ generic[i];
	return any && same ? 0 : -1;
}
#endif
