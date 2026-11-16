// SPDX-License-Identifier: GPL-2.0-only
/* Innioasis Y1 (MT6572) deep idle, docs/planning/Y1-POWER-PLAN.md §17.
 * D6b: the PMIC wrapper's DVFS slots, stock's live table from mt_cpufreq_pdrv_probe (drsami/mt6572_kernel
 * c4b775c): slots 0-5 address VPROC_VOSEL_ON (0x220) with 1.25/1.15/1.05 V and the Y1's 1.15 V for slots
 * 3-5; slots 6/7 TOP_CKPDN0_SET/CLR (0x104/0x106) with 0x2. LK leaves every slot 0, and a request against a
 * zero slot would write PMIC register 0, so the table is written and read back before any request. Linux
 * itself requests only slot 1 (1.15 V, today's voltage), as stock's spm_dvfs_ctrl_volt: AP_DVFS_CON_SET
 * low three bits, then the wrapper's acknowledge in bit 31; VOSEL_ON and the PMIC's own NI_VPROC_VOSEL
 * must then read 0x48. Slot 2 (1.05 V) is left to stock's deep-idle firmware (D6c/D6d). Any mismatch
 * leaves deep idle off for this boot.
 * Blocker B1: stock's deep idle requires the SEJ clock (CTRL1 bit 21) gated; LK leaves it on, nothing in Linux
 * uses it and clk_ignore_unused keeps it on, so it is taken once and released, which gates it.
 * Blocker B5: the same for the MMSYS clocks of stock's mask that LK leaves on and the Y1's display path
 * (OVL, RDMA, COLOR, BLS, DSI) does not use: DISP_WDMA, CMDQ and its SMI interface, MDP_BLS_26M and the DBI
 * engine. The path's own driverless clocks (SMI common, LARB0, BLS) now follow OVL (patches-y1 0019).
 * D6c: cpuidle state 1 on CPU0, stock's spm_go_to_dpidle with cpu_pdn = 0 (mt_spm_dpidle.c, mt_spm_pcm.c,
 * mt_idle.c, mt_cirq.c): the PCM firmware lowers VPROC through DVFS slot 2 while CPU0 waits in WFI with its
 * state kept, and returns it to slot 1 on wake. The state starts disabled (sysfs cpuidle state1/disable). */
#include <dt-bindings/clock/mediatek,mt6572-clk.h>
#include <linux/cpuidle.h> /* before asm/cpuidle.h, which uses its types */
#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/cpuidle.h>
#include <linux/clk.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/tick.h>
#include <linux/workqueue.h>
#include "dpidle-pcm.h"
#include "system-idle.h"
#define PWRAP_BASE 0x1000f000
#define PWRAP_HIPRIO_ARB_EN 0x50
#define PWRAP_DVFS_ADR(n) (0xe4 + (n) * 8)
#define PWRAP_DVFS_WDATA(n) (0xe8 + (n) * 8)
#define PWRAP_ARB_DVFSINF BIT(4)
#define SPM_BASE 0x10006000
#define SPM_AP_DVFS_CON_SET 0x604
#define AP_DVFS_ACK BIT(31)
#define PMIC_VPROC_VOSEL_ON 0x220
#define PMIC_NI_VPROC_VOSEL 0x224
#define VPROC_1150MV 0x48
static const struct { u16 address, data; } y1_dvfs_slots[8] = {
	{ 0x220, 0x58 }, { 0x220, 0x48 }, { 0x220, 0x38 }, { 0x220, 0x48 },
	{ 0x220, 0x48 }, { 0x220, 0x48 }, { 0x104, 0x2 }, { 0x106, 0x2 },
};
static bool slots_ready;
static int slots_error;
static char *slots_stage = "not_attempted";
static unsigned slot_ack_polls, slot_vosel, slot_actual, slot_con;
module_param(slots_ready, bool, 0400);
module_param(slots_error, int, 0400);
module_param(slots_stage, charp, 0400);
module_param(slot_ack_polls, uint, 0400);
module_param(slot_vosel, uint, 0400);
module_param(slot_actual, uint, 0400);
module_param(slot_con, uint, 0400);
static struct regmap *y1_pmic_regmap(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "mediatek,mt6323");
	struct platform_device *pdev;
	struct mt6397_chip *chip;
	if (!np)
		return NULL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return NULL;
	chip = platform_get_drvdata(pdev);
	put_device(&pdev->dev);
	return chip ? chip->regmap : NULL;
}
/* Stock spm_dvfs_ctrl_volt: select, 5 us, then up to 100 polls of 5 us for the acknowledge. */
static int y1_dvfs_request(void __iomem *spm, unsigned slot)
{
	unsigned n, value;
	writel((readl(spm + SPM_AP_DVFS_CON_SET) & ~7U) | slot, spm + SPM_AP_DVFS_CON_SET);
	udelay(5);
	for (n = 0; n <= 100; n++) {
		value = readl(spm + SPM_AP_DVFS_CON_SET);
		slot_con = value;
		slot_ack_polls = n;
		if ((value & 7) != slot)
			return -EIO;
		if (value & AP_DVFS_ACK)
			return 0;
		udelay(5);
	}
	return -ETIMEDOUT;
}
/* The PMIC wrapper's probe can be deferred past late_initcall (D6b's first boot: the MT6323 appeared
 * 5 ms later), so the table is written from a work item that waits up to 10 s for the PMIC. */
#define SLOTS_RETRY_MS 200
#define SLOTS_RETRIES 50
static unsigned slots_tries;
static void y1_deep_idle_slots(struct work_struct *work);
static DECLARE_DELAYED_WORK(slots_work, y1_deep_idle_slots);
module_param(slots_tries, uint, 0400);
static void y1_deep_idle_slots(struct work_struct *work)
{
	void __iomem *wrap = NULL, *spm = NULL;
	struct regmap *pmic;
	unsigned n, value;
	int ret = 0;
	slots_stage = "pmic";
	pmic = y1_pmic_regmap();
	if (!pmic && ++slots_tries < SLOTS_RETRIES) {
		schedule_delayed_work(&slots_work, msecs_to_jiffies(SLOTS_RETRY_MS));
		return;
	}
	if (!pmic) {
		ret = -ENODEV;
		goto out;
	}
	/* Today's voltage first: the table assumes VOSEL_ON at 1.15 V (D0). */
	slots_stage = "vosel_before";
	ret = regmap_read(pmic, PMIC_VPROC_VOSEL_ON, &value);
	if (!ret && (value & 0x7f) != VPROC_1150MV)
		ret = -ERANGE;
	if (ret)
		goto out;
	wrap = ioremap(PWRAP_BASE, 0x200);
	spm = ioremap(SPM_BASE, 0x1000);
	if (!wrap || !spm) {
		ret = -ENOMEM;
		goto unmap;
	}
	slots_stage = "arbiter";
	if (!(readl(wrap + PWRAP_HIPRIO_ARB_EN) & PWRAP_ARB_DVFSINF)) {
		ret = -EOPNOTSUPP;
		goto unmap;
	}
	/* Stock's order: every address, then every value. The selector is not changed while writing. */
	slots_stage = "table";
	for (n = 0; n < 8; n++)
		writel(y1_dvfs_slots[n].address, wrap + PWRAP_DVFS_ADR(n));
	for (n = 0; n < 8; n++)
		writel(y1_dvfs_slots[n].data, wrap + PWRAP_DVFS_WDATA(n));
	for (n = 0; n < 8; n++)
		if (readl(wrap + PWRAP_DVFS_ADR(n)) != y1_dvfs_slots[n].address ||
		    readl(wrap + PWRAP_DVFS_WDATA(n)) != y1_dvfs_slots[n].data) {
			ret = -EIO;
			goto unmap;
		}
	slots_stage = "request_slot1";
	ret = y1_dvfs_request(spm, 1);
	if (ret)
		goto unmap;
	udelay(40);
	slots_stage = "readback";
	ret = regmap_read(pmic, PMIC_VPROC_VOSEL_ON, &slot_vosel);
	if (!ret)
		ret = regmap_read(pmic, PMIC_NI_VPROC_VOSEL, &slot_actual);
	if (!ret && ((slot_vosel & 0x7f) != VPROC_1150MV || (slot_actual & 0x7f) != VPROC_1150MV))
		ret = -EIO;
	if (!ret) {
		slots_stage = "ready";
		smp_store_release(&slots_ready, true);
	}
unmap:
	if (spm)
		iounmap(spm);
	if (wrap)
		iounmap(wrap);
out:
	slots_error = ret;
	if (ret)
		pr_err("y1-deep-idle: DVFS slots not ready at %s: %d; deep idle stays off\n", slots_stage, ret);
	else
		pr_info("y1-deep-idle: DVFS slots ready, slot 1 acknowledged after %u polls, VPROC %#x/%#x\n",
			slot_ack_polls, slot_vosel, slot_actual);
}
static const struct { const char *provider; unsigned id; } y1_unused_clocks[] = {
	{ "mediatek,mt6572-topckgen", CLK_TOP_SEJ },
	{ "mediatek,mt6572-mmsys", CLK_MM_DISP_WDMA },
	{ "mediatek,mt6572-mmsys", CLK_MM_CMDQ },
	{ "mediatek,mt6572-mmsys", CLK_MM_SMI_CMDQ },
	{ "mediatek,mt6572-mmsys", CLK_MM_DISP_MDP_BLS_26M },
	{ "mediatek,mt6572-mmsys", CLK_MM_DISP_DBI_ENGINE },
};
static unsigned unused_released;
module_param(unused_released, uint, 0400);
/* Taken once and released: the clock core then gates a clock LK left on that no driver claims. */
static void __init y1_deep_idle_release_unused(void)
{
	struct of_phandle_args spec = { .args_count = 1 };
	struct clk *clk;
	unsigned n;
	for (n = 0; n < ARRAY_SIZE(y1_unused_clocks); n++) {
		spec.np = of_find_compatible_node(NULL, NULL, y1_unused_clocks[n].provider);
		if (!spec.np)
			continue;
		spec.args[0] = y1_unused_clocks[n].id;
		clk = of_clk_get_from_provider(&spec);
		of_node_put(spec.np);
		if (IS_ERR(clk)) {
			pr_warn("y1-deep-idle: unused clock %s/%u: %ld\n", y1_unused_clocks[n].provider,
				y1_unused_clocks[n].id, PTR_ERR(clk));
			continue;
		}
		if (!clk_prepare_enable(clk)) {
			clk_disable_unprepare(clk);
			unused_released++;
		}
		clk_put(clk);
	}
}
/* ---- D6c: stock's deep idle with the CPU powered ---- */
/* SPM, stock mt_spm.h and mt_spm_pcm.h. */
#define SPM_POWERON_CONFIG_SET 0x000
#define SPM_POWER_ON_VAL0 0x010
#define SPM_POWER_ON_VAL1 0x014
#define SPM_PCM_CON0 0x310
#define SPM_PCM_CON1 0x314
#define SPM_PCM_IM_PTR 0x318
#define SPM_PCM_IM_LEN 0x31c
#define SPM_PCM_REG_DATA_INI 0x320
#define SPM_PCM_MAS_PAUSE_MASK 0x354
#define SPM_PCM_PWR_IO_EN 0x358
#define SPM_PCM_TIMER_VAL 0x35c
#define SPM_PCM_REG12_DATA 0x3b0
#define SPM_PCM_REG13_DATA 0x3b4
#define SPM_PCM_EVENT_REG_STA 0x3c0
#define SPM_PCM_FSM_STA 0x3c4
#define SPM_PCM_SW_INT_CLEAR 0x3e4
#define SPM_CLK_CON 0x400
#define SPM_APMCU_PWRCTL 0x600
#define SPM_AP_STANBY_CON 0x608
#define SPM_PWR_STATUS 0x60c
#define SPM_PWR_STATUS_S 0x610
#define SPM_SLEEP_WAKEUP_EVENT_MASK 0x810
#define SPM_SLEEP_CPU_WAKEUP_EVENT 0x814
#define SPM_PCM_WDT_TIMER_VAL 0x824
#define SPM_SLEEP_ISR_MASK 0x900
#define SPM_SLEEP_ISR_STATUS 0x904
#define SPM_PCM_SRC_REQ 0xb04
#define SPM_CORE_WFI_SEL(n) (0xf00 + (n) * 4)
#define SPM_KEY (0xb16U << 16)
#define CON0_PCM_KICK BIT(0)
#define CON0_IM_KICK BIT(1)
#define CON0_IM_SLEEP_DVS BIT(3)
#define CON0_PCM_SW_RESET BIT(15)
#define CON1_MIF_APBEN BIT(3)
#define CON1_PCM_TIMER_EN BIT(5)
#define CON1_IM_NONRP_EN BIT(6)
#define CON1_PCM_WDT_EN BIT(8)
#define CON1_PCM_WDT_WAKE_MODE BIT(9)
#define PWR_STATUS_FC1 BIT(11)
#define PCM_PWRIO_EN_R0 BIT(0)
#define PCM_PWRIO_EN_R1 BIT(1)
#define PCM_PWRIO_EN_R2 BIT(2)
#define PCM_PWRIO_EN_R7 BIT(7)
#define PCM_RF_SYNC_R0 BIT(16)
#define PCM_RF_SYNC_R7 BIT(23)
#define CC_SYSCLK0_EN_0 BIT(0)
#define CC_SYSCLK0_EN_1 BIT(1)
#define CC_LOCK_INFRA_DCM BIT(5)
#define CC_SRCLKENA_MASK BIT(6)
#define CC_CXO32K_REMOVE_MD BIT(9)
#define CC_DISABLE_DORM_PWR BIT(14)
#define CC_DISABLE_INFRA_PWR BIT(15)
#define CC_SRCLKEN0_EN BIT(16)
#define PCM_EVENT_ABORT 0x10000
#define PCM_EVENT_NORMAL 0x100000
#define R7_UART_CLK_OFF_REQ BIT(0)
#define R13_UART_CLK_OFF_ACK BIT(20)
#define EVENT_VEC(event, resume, imme, pc) ((event) | (resume) << 5 | (imme) << 6 | (pc) << 16)
/* Stock WAKE_SRC_FOR_DPIDLE: KP, GPT, EINT, CCIF, USB_CD, USB_PDN, AFE, MD_WDT, CONN_WDT, CONN, THERM; its
 * SYSPWREQ is removed by SPM_BYPASS_SYSPWREQ, which stock builds with 1. */
#define DPIDLE_WAKE_SRC (BIT(2) | BIT(4) | BIT(5) | BIT(6) | BIT(8) | BIT(10) | BIT(14) | BIT(15) | BIT(16) | \
			 BIT(20) | BIT(21))
/* Clock conditions, stock dpidle_condition_mask (gate bit set = off), and PMIC_SW_CG_AP (CTRL1 bit 20),
 * which stock's before_wfi turns on: the firmware needs the wrapper clock to change VPROC. */
#define TOP_BASE 0x10000000
#define TOP_CLK_CFG_0 0x00
#define TOP_CTRL0 0x20
#define TOP_CTRL1 0x24
#define TOP_PMIC_WRAP BIT(20)
#define AUD_INTBUS_MASK (7U << 27)
#define MMSYS_BASE 0x14000000
static const struct { bool mmsys; u16 offset; u32 mask; } dp_gates[] = {
	{ false, TOP_CTRL0, 0x000f002f }, { false, TOP_CTRL1, 0x80a9fefd },
	{ true, 0x100, 0x003fffff }, { true, 0x110, 0x7f },
};
/* GPT4 is stock's deep-idle wake timer at 13 MHz. Linux's timer-mediatek uses GPT1/GPT2 and acks only GPT1,
 * and all GPTs share one interrupt, so GPT4's own enable is set only while armed. */
#define GPT_BASE 0x10008000
#define GPT_IRQEN 0x00
#define GPT_IRQSTA 0x04
#define GPT_IRQACK 0x08
#define GPT4_CON 0x40
#define GPT4_CLK 0x44
#define GPT4_CMP 0x4c
#define GPT4_IRQ BIT(3)
#define GPT_CON_ENABLE BIT(0)
#define GPT_CON_CLEAR BIT(1)
#define GPT_CON_MODE (3U << 4)
#define GPT_CON_FREERUN (3U << 4)
#define DPIDLE_MIN_NS 4192308LL /* stock dpidle_time_critera: 54 500 GPT ticks */
#define DPIDLE_MAX_NS (300LL * NSEC_PER_SEC)
/* GIC distributor; the SPM's interrupt (SPI 72) is the only one left enabled in deep idle. */
#define GIC_DIST_BASE 0x10211000
#define GICD_ISENABLER 0x100
#define GICD_ICENABLER 0x180
#define GICD_ISPENDR 0x200
#define GICD_ICPENDR 0x280
#define GICD_ICFGR 0xc00
#define GIC_BANKS 5
#define SPM_IRQ 104
/* MT6572 CIRQ, stock mt_cirq.h: channel c is GIC interrupt c + 52; polarity from sysirq's INT_POL_CTL. */
#define CIRQ_BASE 0x10202000
#define SYSIRQ_POL_BASE 0x10200100
#define CIRQ_STA 0x000
#define CIRQ_ACK 0x040
#define CIRQ_MASK 0x080
#define CIRQ_MASK_SET 0x0c0
#define CIRQ_MASK_CLR 0x100
#define CIRQ_SENS 0x140
#define CIRQ_SENS_SET 0x180
#define CIRQ_SENS_CLR 0x1c0
#define CIRQ_POL 0x200
#define CIRQ_POL_SET 0x240
#define CIRQ_POL_CLR 0x280
#define CIRQ_CON 0x300
#define CIRQ_CON_EN BIT(0)
#define CIRQ_CHANNELS 107
#define CIRQ_IRQ_BASE 52
#define CIRQ_WORDS 4
enum dp_reason { DP_CPU, DP_SLOTS, DP_QUIET, DP_CLOCKS, DP_TIMER, DP_PENDING, DP_UART, DP_BROKEN, DP_REASONS };
static void __iomem *dp_spm, *dp_top, *dp_mm, *dp_gpt, *dp_cirq, *dp_dist, *dp_pol;
static phys_addr_t dp_pcm_phys;
static bool dpidle_broken;
static char *dpidle_stage = "off";
static unsigned dpidle_entries, dpidle_normal, dpidle_aborts, dpidle_unknown, dpidle_gpt_wakes;
static unsigned dpidle_uart_acks, dpidle_uart_timeouts, dp_uart_streak, dpidle_cirq_flushed;
static unsigned dpidle_last_wake, dpidle_last_event, dpidle_last_r13, dpidle_last_dvfs, dpidle_last_debug;
static unsigned dpidle_last_fsm;
static unsigned dpidle_refusals[DP_REASONS], dpidle_spm_boot[8], gpt4_boot[2];
module_param(dpidle_broken, bool, 0400);
module_param(dpidle_stage, charp, 0400);
module_param(dpidle_entries, uint, 0400);
module_param(dpidle_normal, uint, 0400);
module_param(dpidle_aborts, uint, 0400);
module_param(dpidle_unknown, uint, 0400);
module_param(dpidle_gpt_wakes, uint, 0400);
module_param(dpidle_uart_acks, uint, 0400);
module_param(dpidle_uart_timeouts, uint, 0400);
module_param(dpidle_cirq_flushed, uint, 0400);
module_param(dpidle_last_wake, uint, 0400);
module_param(dpidle_last_event, uint, 0400);
module_param(dpidle_last_r13, uint, 0400);
module_param(dpidle_last_dvfs, uint, 0400);
module_param(dpidle_last_debug, uint, 0400);
module_param(dpidle_last_fsm, uint, 0400);
/* cpu, slots, quiet, clocks, timer, pending, uart, broken */
module_param_array(dpidle_refusals, uint, NULL, 0400);
/* CLK_CON, PCM_CON1, PWR_IO_EN, POWER_ON_VAL1, IM_PTR, IM_LEN, SLEEP_ISR_MASK, WAKEUP_EVENT_MASK at boot */
module_param_array(dpidle_spm_boot, uint, NULL, 0400);
/* GPT4 CON and CLK at boot, for the ownership check */
module_param_array(gpt4_boot, uint, NULL, 0400);
static inline u32 spm_rd(unsigned reg) { return readl(dp_spm + reg); }
static inline void spm_wr(unsigned reg, u32 value) { writel(value, dp_spm + reg); }
/* A fault latches deep idle off until reboot; CPU0 then only uses WFI. */
static int dp_break(const char *stage)
{
	dpidle_stage = (char *)stage;
	WRITE_ONCE(dpidle_broken, true);
	return -EIO;
}
/* Stock dpidle_can_enter, plus the D6a and D6b conditions of the plan's entry list. */
static int dp_admit(u32 *ticks)
{
	s64 delta;
	unsigned n;
	u32 value;
	if (READ_ONCE(dpidle_broken))
		return DP_BROKEN;
	if (smp_processor_id() || num_online_cpus() != 1 ||
	    ((spm_rd(SPM_PWR_STATUS) | spm_rd(SPM_PWR_STATUS_S)) & PWR_STATUS_FC1))
		return DP_CPU;
	if (!smp_load_acquire(&slots_ready))
		return DP_SLOTS;
	if (!y1_system_idle_quiet())
		return DP_QUIET;
	for (n = 0; n < ARRAY_SIZE(dp_gates); n++) {
		value = readl((dp_gates[n].mmsys ? dp_mm : dp_top) + dp_gates[n].offset);
		if ((value & dp_gates[n].mask) != dp_gates[n].mask)
			return DP_CLOCKS;
	}
	if (readl(dp_top + TOP_CTRL1) & TOP_PMIC_WRAP)
		return DP_CLOCKS;
	delta = ktime_to_ns(ktime_sub(tick_nohz_get_next_hrtimer(), ktime_get()));
	if (delta < DPIDLE_MIN_NS)
		return DP_TIMER;
	*ticks = div_u64(min(delta, DPIDLE_MAX_NS) * 13, 1000);
	return -1;
}
/* The SPM's line is active-low: stock requests it IRQF_TRIGGER_LOW, which sets its sysirq inversion bit. No
 * Linux driver requests it on the Y1, so without the bit the idle line reads pending in the GIC and WFI
 * returns at once (D6c first run: 4.8 million entries, event status 0, none slept). */
#define SPM_POL_WORD ((SPM_IRQ - 32) / 32 * 4)
#define SPM_POL_BIT BIT((SPM_IRQ - 32) % 32)
static u32 dp_pol_saved;
/* Stock mt_irq_mask_all + mt_irq_unmask_for_sleep(SPM0). An enabled interrupt already pending refuses. */
static bool dp_gic_mask(u32 *enable)
{
	unsigned b;
	enable[0] = 0;
	for (b = 1; b < GIC_BANKS; b++) {
		enable[b] = readl(dp_dist + GICD_ISENABLER + b * 4);
		if (readl(dp_dist + GICD_ISPENDR + b * 4) & enable[b])
			return false;
	}
	for (b = 1; b < GIC_BANKS; b++)
		writel(~0U, dp_dist + GICD_ICENABLER + b * 4);
	dp_pol_saved = readl(dp_pol + SPM_POL_WORD);
	writel(dp_pol_saved | SPM_POL_BIT, dp_pol + SPM_POL_WORD);
	dsb(sy);
	writel(BIT(SPM_IRQ % 32), dp_dist + GICD_ICPENDR + SPM_IRQ / 32 * 4);
	writel(BIT(SPM_IRQ % 32), dp_dist + GICD_ISENABLER + SPM_IRQ / 32 * 4);
	dsb(sy);
	return true;
}
static bool dp_gic_restore(const u32 *enable)
{
	unsigned b;
	bool ok = true;
	writel(BIT(SPM_IRQ % 32), dp_dist + GICD_ICENABLER + SPM_IRQ / 32 * 4);
	writel(dp_pol_saved, dp_pol + SPM_POL_WORD);
	dsb(sy);
	writel(BIT(SPM_IRQ % 32), dp_dist + GICD_ICPENDR + SPM_IRQ / 32 * 4);
	for (b = 1; b < GIC_BANKS; b++)
		writel(enable[b], dp_dist + GICD_ISENABLER + b * 4);
	dsb(sy);
	for (b = 1; b < GIC_BANKS; b++)
		if (readl(dp_dist + GICD_ISENABLER + b * 4) != enable[b])
			ok = false;
	return ok && readl(dp_pol + SPM_POL_WORD) == dp_pol_saved;
}
static u32 cirq_valid(unsigned word)
{
	return word < CIRQ_WORDS - 1 ? ~0U : GENMASK(CIRQ_CHANNELS - 1 - (CIRQ_WORDS - 1) * 32, 0);
}
/* Stock mt_cirq_clone_gic + mt_cirq_enable, cloned from the enables saved before masking: stock clones
 * after mt_irq_mask_all, which leaves only the SPM's channel latching. */
static int dp_cirq_clone(const u32 *enable)
{
	u32 cfg[10], pol[4], en[CIRQ_WORDS] = {}, level[CIRQ_WORDS] = {}, pos[CIRQ_WORDS] = {}, valid;
	unsigned c, irq, w;
	for (w = CIRQ_IRQ_BASE / 16; w < 10; w++)
		cfg[w] = readl(dp_dist + GICD_ICFGR + w * 4);
	for (w = 0; w < 4; w++)
		pol[w] = readl(dp_pol + w * 4);
	for (c = 0; c < CIRQ_CHANNELS; c++) {
		irq = c + CIRQ_IRQ_BASE;
		if (enable[irq / 32] & BIT(irq % 32))
			en[c / 32] |= BIT(c % 32);
		if (!(cfg[irq / 16] & (2U << (irq % 16) * 2)))
			level[c / 32] |= BIT(c % 32);
		if (!(pol[(irq - 32) / 32] & BIT((irq - 32) % 32)))
			pos[c / 32] |= BIT(c % 32);
	}
	for (w = 0; w < CIRQ_WORDS; w++) {
		valid = cirq_valid(w);
		writel(valid, dp_cirq + CIRQ_MASK_SET + w * 4);
		writel(level[w], dp_cirq + CIRQ_SENS_SET + w * 4);
		writel(~level[w] & valid, dp_cirq + CIRQ_SENS_CLR + w * 4);
		writel(pos[w], dp_cirq + CIRQ_POL_SET + w * 4);
		writel(~pos[w] & valid, dp_cirq + CIRQ_POL_CLR + w * 4);
		writel(valid, dp_cirq + CIRQ_ACK + w * 4);
		writel(en[w], dp_cirq + CIRQ_MASK_CLR + w * 4);
	}
	dsb(sy);
	for (w = 0; w < CIRQ_WORDS; w++) {
		valid = cirq_valid(w);
		if ((readl(dp_cirq + CIRQ_MASK + w * 4) & valid) != (~en[w] & valid) ||
		    (readl(dp_cirq + CIRQ_SENS + w * 4) & valid) != level[w] ||
		    (readl(dp_cirq + CIRQ_POL + w * 4) & valid) != pos[w])
			return -EIO;
	}
	writel((readl(dp_cirq + CIRQ_CON) | CIRQ_CON_EN) & 7, dp_cirq + CIRQ_CON);
	dsb(sy);
	return readl(dp_cirq + CIRQ_CON) & CIRQ_CON_EN ? 0 : -EIO;
}
/* Stock mt_cirq_flush + mt_cirq_disable: each latched channel becomes pending in the GIC. */
static bool dp_cirq_flush(void)
{
	unsigned w, bit, irq;
	u32 sta;
	for (w = 0; w < CIRQ_WORDS; w++) {
		sta = readl(dp_cirq + CIRQ_STA + w * 4) & cirq_valid(w);
		for (bit = 0; bit < 32; bit++)
			if (sta & BIT(bit)) {
				irq = w * 32 + bit + CIRQ_IRQ_BASE;
				writel(BIT(irq % 32), dp_dist + GICD_ISPENDR + irq / 32 * 4);
				dpidle_cirq_flushed++;
			}
		writel(cirq_valid(w), dp_cirq + CIRQ_ACK + w * 4);
	}
	writel(readl(dp_cirq + CIRQ_CON) & ~CIRQ_CON_EN & 7, dp_cirq + CIRQ_CON);
	dsb(sy);
	return !(readl(dp_cirq + CIRQ_CON) & CIRQ_CON_EN);
}
/* Stock spm_dpidle_before_wfi's request_gpt(GPT4, one-shot, 13 MHz system clock, divider 1). */
static void dp_gpt4_arm(u32 ticks)
{
	writel(0, dp_gpt + GPT4_CON);
	writel(0, dp_gpt + GPT4_CLK);
	writel(GPT4_IRQ, dp_gpt + GPT_IRQACK);
	writel(ticks, dp_gpt + GPT4_CMP);
	writel(readl(dp_gpt + GPT_IRQEN) | GPT4_IRQ, dp_gpt + GPT_IRQEN);
	writel(GPT_CON_CLEAR | GPT_CON_ENABLE, dp_gpt + GPT4_CON);
}
/* Before the GIC is unmasked: a GPT4 status left set would hold the shared GPT line. */
static bool dp_gpt4_disarm(void)
{
	bool fired = readl(dp_gpt + GPT_IRQSTA) & GPT4_IRQ;
	writel(0, dp_gpt + GPT4_CON);
	writel(readl(dp_gpt + GPT_IRQEN) & ~GPT4_IRQ, dp_gpt + GPT_IRQEN);
	writel(GPT4_IRQ, dp_gpt + GPT_IRQACK);
	return fired;
}
/* Stock spm_init_pcm for pcm_config_dpidle: pwrlevel 0 (UART sleep request), no PCM timer or WDT. */
static int dp_pcm_arm(void)
{
	static const u32 vectors[4] = {
		EVENT_VEC(11, 1, 0, 0), EVENT_VEC(12, 1, 0, 15), EVENT_VEC(30, 1, 0, 28), EVENT_VEC(31, 1, 0, 69),
	};
	unsigned n;
	u32 con0, val1;
	spm_wr(SPM_POWERON_CONFIG_SET, SPM_KEY | BIT(0));
	spm_wr(SPM_PCM_CON0, SPM_KEY | CON0_PCM_SW_RESET);
	spm_wr(SPM_PCM_CON0, SPM_KEY);
	spm_wr(SPM_PCM_CON0, SPM_KEY | CON0_IM_SLEEP_DVS);
	spm_wr(SPM_PCM_CON1, SPM_KEY | CON1_IM_NONRP_EN | CON1_MIF_APBEN |
	       (spm_rd(SPM_PCM_CON1) & (CON1_PCM_WDT_EN | CON1_PCM_WDT_WAKE_MODE)));
	spm_wr(SPM_PCM_IM_PTR, dp_pcm_phys);
	spm_wr(SPM_PCM_IM_LEN, Y1_DPIDLE_PCM_WORDS);
	con0 = spm_rd(SPM_PCM_CON0) & 0xffff;
	spm_wr(SPM_PCM_CON0, con0 | SPM_KEY | CON0_IM_KICK);
	spm_wr(SPM_PCM_CON0, con0 | SPM_KEY);
	/* spm_request_uart_to_sleep: WAIT_UART_ACK_TIMES 10 of 10 us; without the ACK nothing is entered. */
	val1 = spm_rd(SPM_POWER_ON_VAL1);
	spm_wr(SPM_POWER_ON_VAL1, val1 | R7_UART_CLK_OFF_REQ);
	for (n = 0; !(spm_rd(SPM_PCM_REG13_DATA) & R13_UART_CLK_OFF_ACK); n++) {
		if (n >= 10) {
			spm_wr(SPM_POWER_ON_VAL1, val1);
			dpidle_uart_timeouts++;
			return ++dp_uart_streak >= 16 ? dp_break("uart_ack") : -EBUSY;
		}
		udelay(10);
	}
	dpidle_uart_acks++;
	dp_uart_streak = 0;
	spm_wr(SPM_PCM_REG_DATA_INI, spm_rd(SPM_POWER_ON_VAL0));
	spm_wr(SPM_PCM_PWR_IO_EN, PCM_RF_SYNC_R0);
	spm_wr(SPM_PCM_PWR_IO_EN, 0);
	spm_wr(SPM_PCM_REG_DATA_INI, spm_rd(SPM_POWER_ON_VAL1));
	spm_wr(SPM_PCM_PWR_IO_EN, PCM_RF_SYNC_R7);
	spm_wr(SPM_PCM_PWR_IO_EN, 0);
	spm_wr(SPM_PCM_REG_DATA_INI, 0);
	for (n = 0; n < 4; n++)
		spm_wr(0x340 + n * 4, vectors[n]);
	for (n = 0; n < 4; n++)
		spm_wr(0x3d0 + n * 4, 0);
	spm_wr(SPM_APMCU_PWRCTL, 1); /* PWR_LV0 */
	/* md_mask and mm_mask unmasked (3), SCU and L2C checked, REDUCE_AND; both cores' WFI selected. */
	spm_wr(SPM_AP_STANBY_CON, 3U << 19 | 3U << 16 | 1U << 4);
	for (n = 0; n < 4; n++)
		spm_wr(SPM_CORE_WFI_SEL(n), n < 2);
	spm_wr(SPM_PCM_TIMER_VAL, ~0U);
	spm_wr(SPM_PCM_CON1, SPM_KEY | (spm_rd(SPM_PCM_CON1) & 0xffff) | CON1_PCM_TIMER_EN);
	spm_wr(SPM_PCM_WDT_TIMER_VAL, 0);
	spm_wr(SPM_PCM_CON1, SPM_KEY | (spm_rd(SPM_PCM_CON1) & 0xffff & ~(CON1_PCM_WDT_EN | CON1_PCM_WDT_WAKE_MODE)));
	spm_wr(SPM_SLEEP_WAKEUP_EVENT_MASK, ~DPIDLE_WAKE_SRC);
	spm_wr(SPM_SLEEP_ISR_MASK, 0x200);
	if (spm_rd(SPM_PCM_IM_PTR) != dp_pcm_phys || spm_rd(SPM_PCM_IM_LEN) != Y1_DPIDLE_PCM_WORDS ||
	    spm_rd(SPM_SLEEP_WAKEUP_EVENT_MASK) != ~DPIDLE_WAKE_SRC ||
	    !(spm_rd(SPM_PCM_CON1) & CON1_PCM_TIMER_EN) || (spm_rd(SPM_PCM_CON1) & CON1_PCM_WDT_EN)) {
		spm_wr(SPM_POWER_ON_VAL1, val1);
		return dp_break("arm_readback");
	}
	return 0;
}
/* Stock spm_kick_pcm: CPU and INFRA/DDRPHY power kept, INFRA DCM locked, R0/R7 (and R1/R2) drive power. */
static void dp_pcm_kick(void)
{
	u32 con0;
	spm_wr(SPM_CLK_CON, (spm_rd(SPM_CLK_CON) & ~(CC_DISABLE_DORM_PWR | CC_DISABLE_INFRA_PWR)) |
	       CC_DISABLE_DORM_PWR | CC_DISABLE_INFRA_PWR | CC_LOCK_INFRA_DCM);
	spm_wr(SPM_PCM_MAS_PAUSE_MASK, ~0U);
	spm_wr(SPM_PCM_PWR_IO_EN, PCM_PWRIO_EN_R0 | PCM_PWRIO_EN_R7 | PCM_PWRIO_EN_R1 | PCM_PWRIO_EN_R2);
	spm_wr(SPM_CLK_CON, spm_rd(SPM_CLK_CON) | CC_SRCLKENA_MASK);
	spm_wr(SPM_CLK_CON, spm_rd(SPM_CLK_CON) | CC_CXO32K_REMOVE_MD);
	spm_wr(SPM_PCM_SRC_REQ, 0);
	con0 = spm_rd(SPM_PCM_CON0) & 0xffff & ~(CON0_PCM_KICK | CON0_IM_KICK);
	spm_wr(SPM_PCM_CON0, con0 | SPM_KEY | CON0_PCM_KICK);
	spm_wr(SPM_PCM_CON0, con0 | SPM_KEY);
}
/* Stock spm_get_wakeup_status. A PCM assert (REG_DATA_INI non-zero) or a slot other than 1 after wake
 * latches deep idle off. */
static int dp_wake_status(void)
{
	dpidle_last_debug = spm_rd(SPM_PCM_REG_DATA_INI);
	dpidle_last_event = spm_rd(SPM_PCM_EVENT_REG_STA);
	dpidle_last_wake = spm_rd(SPM_PCM_REG12_DATA);
	dpidle_last_r13 = spm_rd(SPM_PCM_REG13_DATA);
	dpidle_last_dvfs = spm_rd(SPM_AP_DVFS_CON_SET);
	dpidle_last_fsm = spm_rd(SPM_PCM_FSM_STA);
	if (dpidle_last_debug)
		return dp_break("pcm_assert");
	if (dpidle_last_event & PCM_EVENT_ABORT)
		dpidle_aborts++;
	else if (dpidle_last_event & PCM_EVENT_NORMAL)
		dpidle_normal++;
	else
		dpidle_unknown++;
	if ((dpidle_last_dvfs & 7) != 1)
		return dp_break("dvfs_slot");
	return 0;
}
/* Stock spm_clean_after_wakeup for deep idle (SW_INT_CLEAR 0xd); its CCCI flip-flop workaround is for the
 * modem, which the Y1 does not run. */
static int dp_pcm_clean(void)
{
	spm_wr(SPM_POWER_ON_VAL1, spm_rd(SPM_POWER_ON_VAL1) & ~R7_UART_CLK_OFF_REQ);
	spm_wr(SPM_CLK_CON, spm_rd(SPM_CLK_CON) &
	       ~(CC_SYSCLK0_EN_1 | CC_SYSCLK0_EN_0 | CC_SRCLKEN0_EN | CC_SRCLKENA_MASK));
	spm_wr(SPM_PCM_PWR_IO_EN, 0);
	spm_wr(SPM_CLK_CON, spm_rd(SPM_CLK_CON) & ~CC_LOCK_INFRA_DCM);
	spm_wr(SPM_PCM_CON1, SPM_KEY | (spm_rd(SPM_PCM_CON1) & 0xffff & ~(CON1_PCM_TIMER_EN | CON1_PCM_WDT_EN)));
	spm_wr(SPM_SLEEP_CPU_WAKEUP_EVENT, 0);
	spm_wr(SPM_SLEEP_WAKEUP_EVENT_MASK, ~0U);
	spm_wr(SPM_SLEEP_ISR_MASK, 0x8);
	spm_wr(SPM_SLEEP_ISR_STATUS, 0x8);
	spm_wr(SPM_PCM_SW_INT_CLEAR, 0xd);
	if ((spm_rd(SPM_POWER_ON_VAL1) & R7_UART_CLK_OFF_REQ) || spm_rd(SPM_PCM_PWR_IO_EN) ||
	    (spm_rd(SPM_CLK_CON) & (CC_LOCK_INFRA_DCM | CC_SRCLKENA_MASK)) ||
	    (spm_rd(SPM_PCM_CON1) & (CON1_PCM_TIMER_EN | CON1_PCM_WDT_EN)))
		return dp_break("clean_readback");
	return 0;
}
/* The PCM part of stock's spm_go_to_dpidle with its before_wfi/after_wfi, interrupts already masked. */
static int dp_sleep(u32 ticks)
{
	u32 aud;
	int ret = dp_pcm_arm();
	if (ret)
		return ret;
	dp_pcm_kick();
	aud = readl(dp_top + TOP_CLK_CFG_0);
	writel(aud & ~AUD_INTBUS_MASK, dp_top + TOP_CLK_CFG_0); /* clk26m */
	dp_gpt4_arm(ticks);
	dpidle_entries++;
	isb();
	dsb(sy);
	wfi();
	ret = dp_wake_status();
	if (dp_gpt4_disarm())
		dpidle_gpt_wakes++;
	writel((readl(dp_top + TOP_CLK_CFG_0) & ~AUD_INTBUS_MASK) | (aud & AUD_INTBUS_MASK), dp_top + TOP_CLK_CFG_0);
	return dp_pcm_clean() ?: ret;
}
static int dp_enter(struct cpuidle_device *dev, struct cpuidle_driver *drv, int index)
{
	u32 enable[GIC_BANKS], ticks = 0;
	int reason = dp_admit(&ticks), ret;
	if (reason < 0 && !dp_gic_mask(enable))
		reason = DP_PENDING;
	if (reason >= 0) {
		dpidle_refusals[reason]++;
		cpu_do_idle();
		return 0;
	}
	ret = dp_cirq_clone(enable) ? dp_break("cirq_clone") : dp_sleep(ticks);
	if (ret == -EBUSY)
		dpidle_refusals[DP_UART]++;
	if (!dp_cirq_flush())
		ret = dp_break("cirq_disable");
	if (!dp_gic_restore(enable))
		ret = dp_break("gic_restore");
	if (ret) {
		cpu_do_idle();
		return 0;
	}
	return index;
}
static struct cpuidle_driver y1_idle_driver = {
	.name = "y1_idle",
	.owner = THIS_MODULE,
	.states = {
		ARM_CPUIDLE_WFI_STATE,
		{
			.enter = dp_enter,
			.exit_latency = 300,
			.target_residency = 5000,
			.flags = CPUIDLE_FLAG_TIMER_STOP | CPUIDLE_FLAG_OFF,
			.name = "DPIDLE",
			.desc = "SPM deep idle, CPU on (D6c)",
		},
	},
	.state_count = 2,
};
static void __init dp_unmap(void)
{
	void __iomem **maps[] = { &dp_spm, &dp_top, &dp_mm, &dp_gpt, &dp_cirq, &dp_dist, &dp_pol };
	unsigned n;
	for (n = 0; n < ARRAY_SIZE(maps); n++)
		if (*maps[n]) {
			iounmap(*maps[n]);
			*maps[n] = NULL;
		}
}
static int __init y1_dpidle_init(void)
{
	static const unsigned boot_regs[8] = {
		SPM_CLK_CON, SPM_PCM_CON1, SPM_PCM_PWR_IO_EN, SPM_POWER_ON_VAL1,
		SPM_PCM_IM_PTR, SPM_PCM_IM_LEN, SPM_SLEEP_ISR_MASK, SPM_SLEEP_WAKEUP_EVENT_MASK,
	};
	unsigned n;
	int ret;
	dp_spm = ioremap(SPM_BASE, 0x1000);
	dp_top = ioremap(TOP_BASE, 0x100);
	dp_mm = ioremap(MMSYS_BASE, 0x200);
	dp_gpt = ioremap(GPT_BASE, 0x80);
	dp_cirq = ioremap(CIRQ_BASE, 0x400);
	dp_dist = ioremap(GIC_DIST_BASE, 0x1000);
	dp_pol = ioremap(SYSIRQ_POL_BASE, 0x1c);
	if (!dp_spm || !dp_top || !dp_mm || !dp_gpt || !dp_cirq || !dp_dist || !dp_pol) {
		ret = -ENOMEM;
		goto fail;
	}
	for (n = 0; n < ARRAY_SIZE(boot_regs); n++)
		dpidle_spm_boot[n] = spm_rd(boot_regs[n]);
	/* GPT4 must have no Linux owner. LK leaves it free-running (CON 0x31, read on the B5 image) and stock's
	 * mt_gpt_init resets every GPT; an enabled interrupt or another mode would mean a Linux user. */
	gpt4_boot[0] = readl(dp_gpt + GPT4_CON);
	gpt4_boot[1] = readl(dp_gpt + GPT4_CLK);
	if (((gpt4_boot[0] & GPT_CON_ENABLE) && (gpt4_boot[0] & GPT_CON_MODE) != GPT_CON_FREERUN) ||
	    (readl(dp_gpt + GPT_IRQEN) & GPT4_IRQ) || (readl(dp_cirq + CIRQ_CON) & CIRQ_CON_EN)) {
		ret = -EBUSY;
		goto fail;
	}
	writel(0, dp_gpt + GPT4_CON); /* stock __gpt_reset */
	writel(GPT4_IRQ, dp_gpt + GPT_IRQACK);
	/* The SPM fetches the firmware from DRAM. */
	__cpuc_flush_dcache_area((void *)y1_dpidle_pcm, sizeof(y1_dpidle_pcm));
	dp_pcm_phys = __pa_symbol(y1_dpidle_pcm);
	y1_idle_driver.cpumask = (struct cpumask *)cpumask_of(0);
	ret = cpuidle_register(&y1_idle_driver, NULL);
	if (ret)
		goto fail;
	dpidle_stage = "ready";
	return 0;
fail:
	dp_unmap();
	dpidle_stage = "init_failed";
	pr_err("y1-deep-idle: deep idle state not registered: %d\n", ret);
	return ret;
}
static int __init y1_deep_idle_init(void)
{
	if (!of_machine_is_compatible("innioasis,y1"))
		return -ENODEV;
	y1_deep_idle_release_unused();
	schedule_delayed_work(&slots_work, 0);
	y1_dpidle_init();
	return 0;
}
late_initcall(y1_deep_idle_init);
MODULE_LICENSE("GPL");
