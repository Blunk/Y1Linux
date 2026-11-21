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
 * state kept, and returns it to slot 1 on wake. The state starts disabled (sysfs cpuidle state1/disable).
 * D6d: cpuidle state 2, the same with cpu_pdn = 1 (stock mt_idle.c's only setting, dpidle_cpu_pdn = 1): the
 * firmware powers CPU0 down after its WFI with L2 kept (CA7_CACHE_CONFIG bit 4, mt_dormant.c), and the boot ROM
 * resumes it at BOOT_SLV_ADDR. Stock's cpu_power_down/platform_save_context is replaced by mainline's cpu_pm
 * notifiers (GIC, VFP, arch timer) and cpu_suspend/cpu_resume_arm. Enabled at boot: with the DISP request masked every
 * exit is a normal sleep, and it halved the screen-off voltage slope (Y1-POWER-PLAN.md §17, 2026-11-19).
 * D7b: system suspend (suspend to RAM) with stock's suspend firmware (suspend-pcm.h), CPU0 and L2 powered down,
 * INFRA kept powered, for the Y2's sleep helper: its journal (pm-journal.c), RGU backstop (mtk_wdt overlay) and
 * the SPM state on 10006000.power-controller (Y1-POWER-PLAN.md §18). */
#include <dt-bindings/clock/mediatek,mt6572-clk.h>
#include <linux/cpuidle.h> /* before asm/cpuidle.h, which uses its types */
#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/cpuidle.h>
#include <asm/suspend.h>
#include <linux/clk.h>
#include <linux/cpu_pm.h>
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
#include <linux/stringify.h>
#include <linux/tick.h>
#include <linux/workqueue.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/timekeeping.h>
#include <clocksource/arm_arch_timer.h>
#include "dpidle-pcm.h"
#include "suspend-pcm.h"
#include "pm-journal.h"
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
#define SPM_PCM_TIMER_OUT 0x360
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
/* D6d, stock mt_dormant.c: the boot ROM's resume address for a CPU coming out of reset (SRAMROM), and the
 * CA7 cache configuration whose bit 4 keeps L2 from being invalidated on that reset. */
#define BOOT_SLV_BASE 0x10001400
#define BOOT_SLV_CFG 0x0
#define BOOT_SLV_ADDR 0x4
#define BOOT_SLV_CFG_ENABLE 0x13800001
#define BOOT_SLV_CFG_ON BIT(0) /* the key does not read back: CFG reads 0x1 (devmem after D5's CPU1 boot) */
#define CA7_CACHE_CONFIG 0x10200000
#define CA7_L2_KEEP BIT(4)
enum dp_reason { DP_CPU, DP_SLOTS, DP_QUIET, DP_CLOCKS, DP_TIMER, DP_PENDING, DP_UART, DP_BROKEN, DP_REASONS };
static void __iomem *dp_spm, *dp_top, *dp_mm, *dp_gpt, *dp_cirq, *dp_dist, *dp_pol;
static void __iomem *dp_boot, *dp_cache;
static phys_addr_t dp_pcm_phys;
static bool dpidle_broken;
static char *dpidle_stage = "off";
static unsigned dpidle_entries, dpidle_normal, dpidle_aborts, dpidle_unknown, dpidle_gpt_wakes;
static unsigned dpidle_uart_acks, dpidle_uart_timeouts, dp_uart_streak, dpidle_cirq_flushed;
static unsigned dpidle_last_wake, dpidle_last_event, dpidle_last_r13, dpidle_last_dvfs, dpidle_last_debug;
static unsigned dpidle_last_fsm;
/* Abort diagnosis (§17): PCM_TIMER_OUT (32 kHz ticks the firmware's timer ran) per exit and summed, the sleep
 * ISR status, and how often R12 recorded 26M_SLP (bit 12) or AP_SLEEP (bit 31). */
static unsigned dpidle_last_timer_out, dpidle_timer_ticks, dpidle_last_isr, dpidle_r12_26m_slp, dpidle_r12_ap_sleep;
static unsigned dpidle_refusals[DP_REASONS], dpidle_spm_boot[8], gpt4_boot[2];
/* D6d: entries into cpu_suspend, returns through the boot ROM and cpu_resume (CPU0 was powered down), and WFIs
 * that returned without a reset (the firmware did not take the power; Linux's context is kept). */
static unsigned dpidle_dormant_entries, dpidle_dormant_resumes, dpidle_dormant_wfi_returns;
static int dpidle_dormant_last;
/* AP_STANBY_CON's request masks: md_mask (bits 19-20), mm_mask (bits 16-17: DISP, MFG), the SCU and L2C idle
 * masks (bits 6 and 5, 0 = checked) and the cores whose WFI counts (bits 0-1, written to CORE0/1_WFI_SEL, which
 * read back there). Stock's dpidle value is 0x1b0003. The default masks the DISP request (bit 16): Linux keeps
 * the DIS domain on with the screen dark, and its request held the firmware in its abort loop on every entry
 * (docs/planning/Y1-POWER-PLAN.md §17). Every MMSYS clock is gated before entry (dp_gates). Writable for
 * diagnosis; only these bits are taken, REDUCE_AND (bit 4) stays. */
#define DP_STANBY_MASKS (3U << 19 | 3U << 16 | 3U << 5)
#define DP_STANBY_CORES 3U
#define DP_STANBY_DISP BIT(16)
static unsigned dpidle_stanby = (DP_STANBY_MASKS & ~(3U << 5) & ~DP_STANBY_DISP) | DP_STANBY_CORES;
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
module_param(dpidle_stanby, uint, 0600);
module_param(dpidle_last_timer_out, uint, 0400);
module_param(dpidle_timer_ticks, uint, 0400);
module_param(dpidle_last_isr, uint, 0400);
module_param(dpidle_r12_26m_slp, uint, 0400);
module_param(dpidle_r12_ap_sleep, uint, 0400);
module_param(dpidle_dormant_entries, uint, 0400);
module_param(dpidle_dormant_resumes, uint, 0400);
module_param(dpidle_dormant_wfi_returns, uint, 0400);
module_param(dpidle_dormant_last, int, 0400);
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
/* One of stock's SPM_PCM_CONFIGs, as far as the Y1 uses it: the firmware, its four event vectors, APMCU_PWRCTL,
 * AP_STANBY_CON and the cores whose WFI counts, the PCM timer and watchdog (32 kHz ticks, 0 = off) and the wake
 * sources. */
struct spm_program {
	phys_addr_t phys;
	u32 words, vectors[4], pwrctl, stanby, cores, timer, wdt, wake;
	bool keep_wdt;	/* deep idle: CON1 keeps its watchdog bits (stock spm_reset_and_init_pcm) */
	bool settle;	/* the firmware switches 26 MHz off: stock spm_set_sysclk_settle */
};
/* Stock spm_set_sysclk_settle with SPM_AP_ONLY_SLEEP: settle = 99 (3 ms) less the MD settle. The Y1's MD settle
 * reads 128 (0x80, D7b run 1, 2026-11-21), so stock's unsigned subtraction underflows; the AP part is 0 then and
 * the settle stays the MD's 128 ticks (3.9 ms), longer than stock meant, which only delays the wake. */
#define SPM_CLK_SETTLE 0x100
#define CC_SYSSETTLE_SEL BIT(4)
#define SPM_SYSCLK_SETTLE 99
static int spm_settle(void)
{
	u32 md;
	spm_wr(SPM_CLK_CON, spm_rd(SPM_CLK_CON) | CC_SYSSETTLE_SEL);
	spm_wr(SPM_CLK_SETTLE, 0);
	md = spm_rd(SPM_CLK_SETTLE);
	spm_wr(SPM_CLK_SETTLE, md < SPM_SYSCLK_SETTLE ? SPM_SYSCLK_SETTLE - md : 0);
	return 0;
}
/* Stock spm_init_pcm: -EBUSY without the UART's clock-off ACK, -EIO when a write does not read back; nothing is
 * kicked either way and the UART request is withdrawn. A timer or watchdog value is read back only when set (deep
 * idle's all-ones timer never was). */
static int spm_arm(const struct spm_program *p)
{
	unsigned n;
	u32 con0, con1, val1;
	spm_wr(SPM_POWERON_CONFIG_SET, SPM_KEY | BIT(0));
	if (p->settle && spm_settle())
		return -EIO;
	spm_wr(SPM_PCM_CON0, SPM_KEY | CON0_PCM_SW_RESET);
	spm_wr(SPM_PCM_CON0, SPM_KEY);
	spm_wr(SPM_PCM_CON0, SPM_KEY | CON0_IM_SLEEP_DVS);
	spm_wr(SPM_PCM_CON1, SPM_KEY | CON1_IM_NONRP_EN | CON1_MIF_APBEN |
	       (p->keep_wdt ? spm_rd(SPM_PCM_CON1) & (CON1_PCM_WDT_EN | CON1_PCM_WDT_WAKE_MODE) : 0));
	spm_wr(SPM_PCM_IM_PTR, p->phys);
	spm_wr(SPM_PCM_IM_LEN, p->words);
	con0 = spm_rd(SPM_PCM_CON0) & 0xffff;
	spm_wr(SPM_PCM_CON0, con0 | SPM_KEY | CON0_IM_KICK);
	spm_wr(SPM_PCM_CON0, con0 | SPM_KEY);
	/* spm_request_uart_to_sleep: WAIT_UART_ACK_TIMES 10 of 10 us; without the ACK nothing is entered. */
	val1 = spm_rd(SPM_POWER_ON_VAL1);
	spm_wr(SPM_POWER_ON_VAL1, val1 | R7_UART_CLK_OFF_REQ);
	for (n = 0; !(spm_rd(SPM_PCM_REG13_DATA) & R13_UART_CLK_OFF_ACK); n++) {
		if (n >= 10) {
			spm_wr(SPM_POWER_ON_VAL1, val1);
			return -EBUSY;
		}
		udelay(10);
	}
	spm_wr(SPM_PCM_REG_DATA_INI, spm_rd(SPM_POWER_ON_VAL0));
	spm_wr(SPM_PCM_PWR_IO_EN, PCM_RF_SYNC_R0);
	spm_wr(SPM_PCM_PWR_IO_EN, 0);
	spm_wr(SPM_PCM_REG_DATA_INI, spm_rd(SPM_POWER_ON_VAL1));
	spm_wr(SPM_PCM_PWR_IO_EN, PCM_RF_SYNC_R7);
	spm_wr(SPM_PCM_PWR_IO_EN, 0);
	spm_wr(SPM_PCM_REG_DATA_INI, 0);
	for (n = 0; n < 4; n++)
		spm_wr(0x340 + n * 4, p->vectors[n]);
	for (n = 0; n < 4; n++)
		spm_wr(0x3d0 + n * 4, 0);
	spm_wr(SPM_APMCU_PWRCTL, p->pwrctl);
	/* CORE0/1_WFI_SEL read back as AP_STANBY_CON bits 0 and 1 (devmem, 2026-11-17). */
	spm_wr(SPM_AP_STANBY_CON, p->stanby);
	for (n = 0; n < 4; n++)
		spm_wr(SPM_CORE_WFI_SEL(n), n < 2 && (p->cores & BIT(n)));
	/* Stock spm_set_wakeup_event: the timer always runs ("Fix 32K less": TIMER_VAL is all ones unless a wake
	 * period is wanted); the watchdog only with a value, then in wake mode. */
	spm_wr(SPM_PCM_TIMER_VAL, p->timer ?: ~0U);
	spm_wr(SPM_PCM_CON1, SPM_KEY | (spm_rd(SPM_PCM_CON1) & 0xffff) | CON1_PCM_TIMER_EN);
	spm_wr(SPM_PCM_WDT_TIMER_VAL, p->wdt);
	con1 = spm_rd(SPM_PCM_CON1) & 0xffff & ~(CON1_PCM_WDT_EN | CON1_PCM_WDT_WAKE_MODE);
	spm_wr(SPM_PCM_CON1, SPM_KEY | con1 | (p->wdt ? CON1_PCM_WDT_EN | CON1_PCM_WDT_WAKE_MODE : 0));
	spm_wr(SPM_SLEEP_WAKEUP_EVENT_MASK, ~p->wake);
	spm_wr(SPM_SLEEP_ISR_MASK, 0x200);
	if (spm_rd(SPM_PCM_IM_PTR) != p->phys || spm_rd(SPM_PCM_IM_LEN) != p->words ||
	    spm_rd(SPM_SLEEP_WAKEUP_EVENT_MASK) != ~p->wake ||
	    spm_rd(SPM_AP_STANBY_CON) != (p->stanby | (p->cores & DP_STANBY_CORES)) ||
	    (p->timer && spm_rd(SPM_PCM_TIMER_VAL) != p->timer) ||
	    (p->wdt && spm_rd(SPM_PCM_WDT_TIMER_VAL) != p->wdt) ||
	    !(spm_rd(SPM_PCM_CON1) & CON1_PCM_TIMER_EN) || !(spm_rd(SPM_PCM_CON1) & CON1_PCM_WDT_EN) != !p->wdt) {
		spm_wr(SPM_POWER_ON_VAL1, val1);
		return -EIO;
	}
	return 0;
}
/* Stock spm_init_pcm for pcm_config_dpidle: pwrlevel 0 (UART sleep request), no PCM timer or WDT. */
static int dp_pcm_arm(void)
{
	u32 req = READ_ONCE(dpidle_stanby);
	/* The masks and cores from dpidle_stanby (stock: md and mm 3 and 3, SCU and L2C checked, both cores),
	 * REDUCE_AND. */
	const struct spm_program dpidle = {
		.phys = dp_pcm_phys, .words = Y1_DPIDLE_PCM_WORDS,
		.vectors = { EVENT_VEC(11, 1, 0, 0), EVENT_VEC(12, 1, 0, 15), EVENT_VEC(30, 1, 0, 28),
			     EVENT_VEC(31, 1, 0, 69) },
		.pwrctl = 1, /* PWR_LV0 */
		.stanby = (req & DP_STANBY_MASKS) | 1U << 4, .cores = req & DP_STANBY_CORES,
		.wake = DPIDLE_WAKE_SRC, .keep_wdt = true,
	};
	int ret = spm_arm(&dpidle);
	if (ret == -EBUSY) {
		dpidle_uart_timeouts++;
		return ++dp_uart_streak >= 16 ? dp_break("uart_ack") : -EBUSY;
	}
	if (ret)
		return dp_break("arm_readback");
	dpidle_uart_acks++;
	dp_uart_streak = 0;
	return 0;
}
/* Stock spm_kick_pcm: INFRA/DDRPHY power kept, CPU power kept unless cpu_pdn (D6d), INFRA DCM locked, R0/R7
 * (and R1/R2) drive power. */
static void dp_pcm_kick(bool pdn)
{
	u32 con0;
	spm_wr(SPM_CLK_CON, (spm_rd(SPM_CLK_CON) & ~(CC_DISABLE_DORM_PWR | CC_DISABLE_INFRA_PWR)) |
	       (pdn ? 0 : CC_DISABLE_DORM_PWR) | CC_DISABLE_INFRA_PWR | CC_LOCK_INFRA_DCM);
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
	dpidle_last_timer_out = spm_rd(SPM_PCM_TIMER_OUT);
	dpidle_timer_ticks += dpidle_last_timer_out;
	dpidle_last_isr = spm_rd(SPM_SLEEP_ISR_STATUS);
	dpidle_r12_26m_slp += !!(dpidle_last_wake & BIT(12));
	dpidle_r12_ap_sleep += !!(dpidle_last_wake & BIT(31));
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
/* Stock spm_clean_after_wakeup (SW_INT_CLEAR 0xd for deep idle, 0xf for suspend); its CCCI flip-flop workaround
 * is for the modem, which the Y1 does not run. False when a cleared bit is still set. */
static bool spm_clean(u32 sw_int_clear)
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
	spm_wr(SPM_PCM_SW_INT_CLEAR, sw_int_clear);
	return !((spm_rd(SPM_POWER_ON_VAL1) & R7_UART_CLK_OFF_REQ) || spm_rd(SPM_PCM_PWR_IO_EN) ||
		 (spm_rd(SPM_CLK_CON) & (CC_LOCK_INFRA_DCM | CC_SRCLKENA_MASK)) ||
		 (spm_rd(SPM_PCM_CON1) & (CON1_PCM_TIMER_EN | CON1_PCM_WDT_EN)));
}
static int dp_pcm_clean(void)
{
	return spm_clean(0xd) ? 0 : dp_break("clean_readback");
}
/* D6d finisher, after cpu_suspend has saved Linux's context: v7_exit_coherency_flush(all) with the flush called
 * through a register (clang's integrated assembler rejects the macro's "bl", as in patches-y1 0044), then stock's
 * clrex and WFI. The firmware powers CPU0 down in that WFI and the boot ROM resumes it at cpu_resume_arm. If the
 * WFI returns instead, the power was not taken: coherency and the data cache come back on and cpu_suspend
 * returns -EBUSY with the context intact (stock cpu_check_dormant_abort). */
extern void v7_flush_dcache_all(void);
static noinline int dp_power_off(void)
{
	asm volatile(
	"mrc	p15, 0, r0, c1, c0, 0\n\t"
	"bic	r0, r0, #" __stringify(CR_C) "\n\t"
	"mcr	p15, 0, r0, c1, c0, 0\n\t"
	"isb\n\t"
	"blx	%0\n\t"
	"mrc	p15, 0, r0, c1, c0, 1\n\t"
	"bic	r0, r0, #(1 << 6)\n\t"
	"mcr	p15, 0, r0, c1, c0, 1\n\t"
	"isb\n\t"
	"dsb\n\t"
	"clrex\n\t"
	"dsb\n\t"
	"wfi\n\t"
	"mrc	p15, 0, r0, c1, c0, 1\n\t"
	"orr	r0, r0, #(1 << 6)\n\t"
	"mcr	p15, 0, r0, c1, c0, 1\n\t"
	"isb\n\t"
	"mrc	p15, 0, r0, c1, c0, 0\n\t"
	"orr	r0, r0, #" __stringify(CR_C) "\n\t"
	"mcr	p15, 0, r0, c1, c0, 0\n\t"
	"isb"
	: : "r" (v7_flush_dcache_all)
	: "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r9", "r10", "ip", "lr", "memory");
	return -EBUSY;
}
static int dp_finish(unsigned long unused)
{
	return dp_power_off();
}
/* D6d: stock cpu_power_down's boot vector and L2 retention bit, read back before the PCM is armed. */
static int dp_dormant_arm(u32 cache)
{
	u32 entry = __pa_symbol(cpu_resume_arm);
	writel(BOOT_SLV_CFG_ENABLE, dp_boot + BOOT_SLV_CFG);
	writel(entry, dp_boot + BOOT_SLV_ADDR);
	writel(cache | CA7_L2_KEEP, dp_cache);
	dsb(sy);
	if (readl(dp_boot + BOOT_SLV_CFG) != BOOT_SLV_CFG_ON ||
	    readl(dp_boot + BOOT_SLV_ADDR) != entry || readl(dp_cache) != (cache | CA7_L2_KEEP))
		return dp_break("dormant_arm");
	return 0;
}
/* The WFI itself: plain for D6c; for D6d through cpu_suspend with RCU told the CPU is idle (the state carries
 * CPUIDLE_FLAG_RCU_IDLE so that the cpu_pm notifiers run with RCU watching). */
static int dp_wfi(bool pdn)
{
	int ret;
	if (!pdn) {
		isb();
		dsb(sy);
		wfi();
		return 0;
	}
	dpidle_dormant_entries++;
	ct_cpuidle_enter();
	ret = cpu_suspend(0, dp_finish);
	ct_cpuidle_exit();
	dpidle_dormant_last = ret;
	if (!ret)
		dpidle_dormant_resumes++;
	else if (ret == -EBUSY)
		dpidle_dormant_wfi_returns++;
	else
		return dp_break("cpu_suspend");
	return 0;
}
/* The PCM part of stock's spm_go_to_dpidle with its before_wfi/after_wfi, interrupts already masked. */
static int dp_sleep(u32 ticks, bool pdn)
{
	u32 aud;
	int ret = dp_pcm_arm(), wfi_ret;
	if (ret)
		return ret;
	dp_pcm_kick(pdn);
	aud = readl(dp_top + TOP_CLK_CFG_0);
	writel(aud & ~AUD_INTBUS_MASK, dp_top + TOP_CLK_CFG_0); /* clk26m */
	dp_gpt4_arm(ticks);
	dpidle_entries++;
	wfi_ret = dp_wfi(pdn);
	ret = dp_wake_status() ?: wfi_ret;
	if (dp_gpt4_disarm())
		dpidle_gpt_wakes++;
	writel((readl(dp_top + TOP_CLK_CFG_0) & ~AUD_INTBUS_MASK) | (aud & AUD_INTBUS_MASK), dp_top + TOP_CLK_CFG_0);
	return dp_pcm_clean() ?: ret;
}
/* D6d around dp_sleep: Linux's CPU and cluster context (the GIC distributor holds the masked enables here, which
 * its restore puts back before dp_gic_restore), the boot vector and L2 bit, and the L2 bit cleared again on
 * either return (stock platform_restore_context and cpu_check_dormant_abort). */
static int dp_sleep_dormant(u32 ticks)
{
	u32 cache = readl(dp_cache);
	int ret = cpu_pm_enter();
	if (ret) {
		dpidle_dormant_last = ret; /* a notifier refused: plain WFI, not counted as a UART refusal */
		return -EAGAIN;
	}
	ret = cpu_cluster_pm_enter();
	if (!ret) {
		ret = dp_dormant_arm(cache) ?: dp_sleep(ticks, true);
		writel(cache, dp_cache);
		dsb(sy);
		if (readl(dp_cache) != cache)
			ret = dp_break("dormant_cache");
		cpu_cluster_pm_exit();
	} else {
		dpidle_dormant_last = ret;
		ret = -EAGAIN;
	}
	cpu_pm_exit();
	return ret;
}
/* Plain WFI in place of the state; a D6d state runs with RCU watching, so it says the CPU is idle around it. */
static void dp_idle(bool pdn)
{
	if (pdn)
		ct_cpuidle_enter();
	cpu_do_idle();
	if (pdn)
		ct_cpuidle_exit();
}
static int dp_enter_common(int index, bool pdn)
{
	u32 enable[GIC_BANKS], ticks = 0;
	int reason = dp_admit(&ticks), ret;
	if (reason < 0 && !dp_gic_mask(enable))
		reason = DP_PENDING;
	if (reason >= 0) {
		dpidle_refusals[reason]++;
		dp_idle(pdn);
		return 0;
	}
	if (dp_cirq_clone(enable))
		ret = dp_break("cirq_clone");
	else
		ret = pdn ? dp_sleep_dormant(ticks) : dp_sleep(ticks, false);
	if (ret == -EBUSY)
		dpidle_refusals[DP_UART]++;
	if (!dp_cirq_flush())
		ret = dp_break("cirq_disable");
	if (!dp_gic_restore(enable))
		ret = dp_break("gic_restore");
	if (ret) {
		dp_idle(pdn);
		return 0;
	}
	return index;
}
static int dp_enter(struct cpuidle_device *dev, struct cpuidle_driver *drv, int index)
{
	return dp_enter_common(index, false);
}
static int dp_enter_dormant(struct cpuidle_device *dev, struct cpuidle_driver *drv, int index)
{
	return dp_enter_common(index, true);
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
		{
			.enter = dp_enter_dormant,
			.exit_latency = 600,
			.target_residency = 5000,
			.flags = CPUIDLE_FLAG_TIMER_STOP | CPUIDLE_FLAG_RCU_IDLE,
			.name = "DORMANT",
			.desc = "SPM deep idle, CPU0 off (D6d)",
		},
	},
	.state_count = 3,
};
/* ---- D7b: system suspend, stock spm_go_to_sleep with INFRA kept powered (Y1-POWER-PLAN.md §18) ---- */
#if IS_ENABLED(CONFIG_INNIOASIS_Y1_SLEEP)
extern void y1_cpu_resume(void); /* resume-vector.S */
/* Stock pcm_config_suspend (mt_spm_suspend.c): its vectors, PWR_LVNA (APMCU_PWRCTL 0xff), md_mask MDCONN_UNMASK
 * and mm_mask MMALL_MASK with the SCU and L2C masks and REDUCE_AND (AP_STANBY_CON 0x180070), both cores' WFI,
 * the 26 MHz settle, cpu_pdn. INFRA stays powered (stock's infra_pdn = 1 is a later step, D7d). Wake: stock's KP,
 * EINT, USB_CD and THERM; its CCIF, MD_WDT and CONN belong to the modem and the radios, which are off, and SYSPWREQ
 * is bypassed as in stock. The PCM timer and watchdog are the Y2's (kernel/platform/spm-suspend-policy.h): stock
 * arms neither, so the maintenance wake bounds every sleep and the PCM watchdog wakes 30 s after it. */
#define SLEEP_WAKE_SRC (BIT(2) | BIT(5) | BIT(14) | BIT(21))
#define SLEEP_STANBY 0x180070
#define SLEEP_WDT_MARGIN (30U * 32768U)
#define SLEEP_MAINTENANCE_MAX 1800U
static phys_addr_t sp_pcm_phys;
static bool sleep_ready, sleep_broken, sleep_cirq_pending;
/* On at boot since D7b runs 2-6 (2026-11-21: timer, button and cable wakes, the product path). Writing 0 makes
 * the sleep helper see system_sleep_abi=0, which refuses every sleep, Reborn's automatic one included. */
static bool sleep_enabled = true;
static char *sleep_stage = "off";
/* The maintenance wake (s): the Y2's 30 min; shorter for an owner's test of the timer wake. */
static unsigned sleep_maintenance_s = SLEEP_MAINTENANCE_MAX;
static unsigned sleep_entries, sleep_resumes, sleep_wfi_returns, sleep_aborts, sleep_normal, sleep_unknown;
static unsigned sleep_uart_busy, sleep_refused_pending, sleep_restore_failures;
static unsigned sleep_last_wake, sleep_last_event, sleep_last_debug, sleep_last_r13, sleep_last_isr;
static unsigned sleep_last_cpu_wake, sleep_last_ticks, sleep_last_dvfs, sleep_last_fsm;
static int sleep_last_result;
static u32 sleep_enable[GIC_BANKS];
/* Sleep time. The arch counter does not count the sleep (D7b runs 2 and 3, 2026-11-21: the clocks lost 60 s
 * sleeps), so the DTS marks it arm,no-tick-in-suspend and timekeeping takes no time from it; every completed sleep
 * adds the PCM timer's span instead (sleep_wake). The counter Linux reads (arch_timer_read_counter) before and
 * after is recorded for diagnosis only. */
static u64 sleep_counter_ns, sleep_pcm_ns, sleep_injected_ns, sleep_counter_before, sleep_counter_after;
static unsigned sleep_injections;
static int sleep_maintenance_set(const char *value, const struct kernel_param *kp)
{
	unsigned s;
	int ret = kstrtouint(value, 0, &s);
	if (ret)
		return ret;
	if (s < 10 || s > SLEEP_MAINTENANCE_MAX)
		return -ERANGE;
	WRITE_ONCE(sleep_maintenance_s, s);
	return 0;
}
static const struct kernel_param_ops sleep_maintenance_ops = {
	.set = sleep_maintenance_set, .get = param_get_uint,
};
module_param_cb(sleep_maintenance_s, &sleep_maintenance_ops, &sleep_maintenance_s, 0644);
module_param(sleep_enabled, bool, 0644);
module_param(sleep_broken, bool, 0400);
module_param(sleep_stage, charp, 0400);
static int sleep_break(const char *stage)
{
	sleep_stage = (char *)stage;
	WRITE_ONCE(sleep_broken, true);
	return -EIO;
}
/* Stock spm_go_to_sleep's preconditions (spm_cpusys_can_power_down) and the firmware's: one CPU, CPU1 powered
 * off, the DVFS table it requests slots 4-7 from, and the PMIC wrapper's clock (its sleep path writes the PMIC). */
static int sleep_admit(void)
{
	if (READ_ONCE(sleep_broken))
		return -EIO;
	if (smp_processor_id() || num_online_cpus() != 1 ||
	    ((spm_rd(SPM_PWR_STATUS) | spm_rd(SPM_PWR_STATUS_S)) & PWR_STATUS_FC1))
		return -EBUSY;
	if (!smp_load_acquire(&slots_ready) || (readl(dp_top + TOP_CTRL1) & TOP_PMIC_WRAP))
		return -EBUSY;
	return 0;
}
/* Stock's SHUTDOWN_MODE: the boot vector, and CA7_CACHE_CONFIG bit 4 clear so that the reset invalidates L2,
 * which the finisher has flushed (stock sets the bit only for DORMANT). */
static int sleep_vector_arm(u32 cache)
{
	u32 entry = __pa_symbol(y1_cpu_resume);
	writel(BOOT_SLV_CFG_ENABLE, dp_boot + BOOT_SLV_CFG);
	writel(entry, dp_boot + BOOT_SLV_ADDR);
	writel(cache & ~CA7_L2_KEEP, dp_cache);
	dsb(sy);
	if (readl(dp_boot + BOOT_SLV_CFG) != BOOT_SLV_CFG_ON ||
	    readl(dp_boot + BOOT_SLV_ADDR) != entry || readl(dp_cache) != (cache & ~CA7_L2_KEEP))
		return sleep_break("vector_arm");
	return 0;
}
static void sleep_journal_snapshot(void)
{
	y2_pm_entry_snapshot(spm_rd(SPM_PCM_TIMER_VAL), spm_rd(SPM_PCM_WDT_TIMER_VAL), spm_rd(SPM_PCM_CON1),
			     spm_rd(SPM_CLK_CON), readl(dp_cache));
	y2_pm_spm_snapshot(spm_rd(SPM_PCM_REG12_DATA), spm_rd(SPM_PCM_REG13_DATA), spm_rd(SPM_SLEEP_ISR_STATUS),
			   spm_rd(SPM_PCM_IM_PTR), spm_rd(SPM_PCM_IM_LEN), spm_rd(SPM_SLEEP_WAKEUP_EVENT_MASK),
			   readl(dp_boot + BOOT_SLV_ADDR), readl(dp_boot + BOOT_SLV_CFG),
			   readl(dp_cirq + CIRQ_CON), spm_rd(SPM_PCM_FSM_STA), spm_rd(SPM_PWR_STATUS),
			   spm_rd(SPM_PWR_STATUS_S));
}
/* After cpu_suspend saved Linux's context (the Y2's y2_spm_finish): the last retained mark, the RGU backstop
 * paused (it must not count sleep time), then the D6d power-off path. */
static int sleep_finish(unsigned long unused)
{
	sleep_journal_snapshot();
	y2_pm_mark(Y2_PM_BEFORE_SPM_ENTRY, 0);
	y2_pm_backstop_pause(true);
	return dp_power_off();
}
/* Stock spm_get_wakeup_status for suspend. A PCM assert latches sleep off. */
static int sleep_wake_status(void)
{
	sleep_last_debug = spm_rd(SPM_PCM_REG_DATA_INI);
	sleep_last_event = spm_rd(SPM_PCM_EVENT_REG_STA);
	sleep_last_wake = spm_rd(SPM_PCM_REG12_DATA);
	sleep_last_r13 = spm_rd(SPM_PCM_REG13_DATA);
	sleep_last_isr = spm_rd(SPM_SLEEP_ISR_STATUS);
	sleep_last_cpu_wake = spm_rd(SPM_SLEEP_CPU_WAKEUP_EVENT);
	sleep_last_ticks = spm_rd(SPM_PCM_TIMER_OUT);
	sleep_last_dvfs = spm_rd(SPM_AP_DVFS_CON_SET);
	sleep_last_fsm = spm_rd(SPM_PCM_FSM_STA);
	if (sleep_last_debug)
		return sleep_break("pcm_assert");
	if (sleep_last_event & PCM_EVENT_ABORT)
		sleep_aborts++;
	else if (sleep_last_event & PCM_EVENT_NORMAL)
		sleep_normal++;
	else
		sleep_unknown++;
	return 0;
}
/* Arm, kick, sleep, and undo, with the GIC already masked and CIRQ cloned. The firmware's last DVFS request may
 * be one of its slots 4-7; Linux asks for slot 1 again, as every deep-idle exit leaves it. */
static int sleep_spm(void)
{
	const u32 timer = READ_ONCE(sleep_maintenance_s) * 32768U;
	const struct spm_program suspend = {
		.phys = sp_pcm_phys, .words = Y1_SUSPEND_PCM_WORDS,
		.vectors = { EVENT_VEC(11, 1, 0, 0), EVENT_VEC(12, 1, 0, 33), EVENT_VEC(30, 1, 0, 61),
			     EVENT_VEC(31, 1, 0, 113) },
		.pwrctl = 0xff, .stanby = SLEEP_STANBY, .cores = DP_STANBY_CORES,
		.timer = timer, .wdt = timer + SLEEP_WDT_MARGIN, .wake = SLEEP_WAKE_SRC, .settle = true,
	};
	u32 cache = readl(dp_cache);
	u64 before, after;
	int ret = sleep_vector_arm(cache), wfi;
	if (ret)
		goto cache;
	ret = spm_arm(&suspend);
	sleep_journal_snapshot();
	if (ret == -EBUSY)
		sleep_uart_busy++;
	else if (ret)
		ret = sleep_break("arm_readback");
	if (ret) {
		/* Nothing was kicked, but the timer and watchdog may be armed: stock's clean undoes them. */
		if (!spm_clean(0xf))
			ret = sleep_break("clean_readback");
		goto cache;
	}
	y2_pm_mark(Y2_PM_PCM_INSTALLED, 0);
	y2_pm_mark(Y2_PM_WAKE_MASK_PROGRAMMED, 0);
	y2_pm_mark(Y2_PM_CPU_CONTEXT_SAVING, 0);
	dp_pcm_kick(true);
	before = arch_timer_read_counter();
	wfi = cpu_suspend(0, sleep_finish);
	after = arch_timer_read_counter();
	y2_pm_backstop_pause(false);
	if (!wfi)
		sleep_resumes++;
	else if (wfi == -EBUSY)
		sleep_wfi_returns++;
	ret = sleep_wake_status();
	sleep_journal_snapshot(); /* the wake's registers, before the clean below */
	if (!ret && wfi && wfi != -EBUSY)
		ret = sleep_break("cpu_suspend");
	y2_pm_mark(Y2_PM_AFTER_SPM_RETURN, ret ?: wfi);
	y2_pm_mark(Y2_PM_CPU_CONTEXT_RESTORED, ret ?: wfi);
	sleep_counter_before = before;
	sleep_counter_after = after;
	sleep_counter_ns = after < before ? 0 :
		mul_u64_u32_div(after - before, NSEC_PER_SEC, arch_timer_get_rate() ?: 1);
	sleep_pcm_ns = div_u64((u64)sleep_last_ticks * NSEC_PER_SEC, 32768);
	if (y1_dvfs_request(dp_spm, 1))
		ret = sleep_break("dvfs_slot");
	if (!spm_clean(0xf))
		ret = sleep_break("clean_readback");
	if (!ret)
		ret = wfi;
cache:
	writel(cache, dp_cache);
	dsb(sy);
	if (readl(dp_cache) != cache)
		ret = sleep_break("cache_restore");
	return ret;
}
/* suspend_ops->enter, after syscore_suspend saved the GIC, VFP and arch-timer context (cpu_pm): no cpu_pm calls
 * here, as on the Y2 (a second save after the masking below would overwrite the saved enables). The CIRQ replay
 * waits for that context's restore (sleep_syscore_resume). */
static int sleep_enter(suspend_state_t state)
{
	int ret;
	if (state != PM_SUSPEND_MEM)
		return -EINVAL;
	ret = sleep_admit();
	if (ret)
		goto out;
	if (!dp_gic_mask(sleep_enable)) {
		sleep_refused_pending++;
		ret = -EBUSY;
		goto out;
	}
	sleep_entries++;
	sleep_cirq_pending = true;
	ret = dp_cirq_clone(sleep_enable) ? sleep_break("cirq_clone") : 0;
	y2_pm_mark(Y2_PM_CIRQ_CLONED, ret);
	if (!ret)
		ret = sleep_spm();
	/* The SPM line's polarity and enable; every other enable comes back with the cpu_pm restore. */
	writel(BIT(SPM_IRQ % 32), dp_dist + GICD_ICENABLER + SPM_IRQ / 32 * 4);
	writel(dp_pol_saved, dp_pol + SPM_POL_WORD);
	dsb(sy);
	writel(BIT(SPM_IRQ % 32), dp_dist + GICD_ICPENDR + SPM_IRQ / 32 * 4);
out:
	sleep_last_result = ret;
	if (ret)
		y2_pm_mark(Y2_PM_ABORTED, ret);
	return ret;
}
/* After the cpu_pm syscore restored the distributor (this syscore registers later, so it resumes after it):
 * latched wakes become pending in the GIC, CIRQ goes off, and the restored enables must equal those at entry. */
static void sleep_syscore_resume(void *data)
{
	unsigned b;
	int ret = 0;
	if (!sleep_cirq_pending)
		return;
	sleep_cirq_pending = false;
	if (!dp_cirq_flush())
		ret = sleep_break("cirq_disable");
	for (b = 1; b < GIC_BANKS; b++)
		if (readl(dp_dist + GICD_ISENABLER + b * 4) != sleep_enable[b])
			ret = sleep_break("gic_restore");
	if (readl(dp_pol + SPM_POL_WORD) != dp_pol_saved)
		ret = sleep_break("gic_polarity");
	if (ret)
		sleep_restore_failures++;
	y2_pm_mark(Y2_PM_CIRQ_REPLAYED, ret);
}
static const struct syscore_ops sleep_syscore_ops = { .resume = sleep_syscore_resume };
static struct syscore sleep_syscore = { .ops = &sleep_syscore_ops };
/* suspend_ops->wake, with interrupts on: the PCM timer's span of a completed sleep goes to the clocks. */
static void sleep_wake(void)
{
	struct timespec64 delta;
	if (sleep_last_result || !sleep_pcm_ns)
		return;
	sleep_injected_ns = sleep_pcm_ns;
	sleep_injections++;
	delta = ns_to_timespec64(sleep_injected_ns);
	timekeeping_inject_sleeptime64(&delta);
}
static int sleep_prepare(void)
{
	if (!READ_ONCE(sleep_ready) || READ_ONCE(sleep_broken))
		return -EIO;
	return READ_ONCE(sleep_enabled) && y2_pm_journal_ready() ? 0 : -EOPNOTSUPP;
}
static const struct platform_suspend_ops sleep_ops = {
	.valid = suspend_valid_only_mem,
	.prepare = sleep_prepare,
	.enter = sleep_enter,
	.wake = sleep_wake,
};
/* The SPM state the shared sleep helper reads (the Y2's kernel/platform/spm.c state_show fields it uses:
 * system_sleep_abi, broken, maintenance_seconds, timer_wake, ticks32k, wake), on 10006000.power-controller. A
 * timer wake is a normal exit whose only wake bit is the PCM timer (bit 0) after the full maintenance span. */
static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned maintenance = READ_ONCE(sleep_maintenance_s);
	bool timer_wake = !sleep_last_result && !sleep_last_debug && sleep_last_wake == 1 &&
		(sleep_last_event & PCM_EVENT_NORMAL) && sleep_last_ticks >= maintenance * 32768U;
	ssize_t n;
	n = sysfs_emit(buf, "mode=SPM_CPU_SHUTDOWN_INFRA_RETAINED broken=%u stage=%s entries=%u resumes=%u wfi_returns=%u aborts=%u normal=%u unknown=%u result=%d\n",
		       READ_ONCE(sleep_broken), sleep_stage, sleep_entries, sleep_resumes, sleep_wfi_returns,
		       sleep_aborts, sleep_normal, sleep_unknown, sleep_last_result);
	n += sysfs_emit_at(buf, n, "wake=%#x ticks32k=%u event=%#x debug=%#x r13=%#x sleep_isr=%#x cpu_wake=%#x dvfs=%#x fsm=%#x power=%#x/%#x\n",
			   sleep_last_wake, sleep_last_ticks, sleep_last_event, sleep_last_debug, sleep_last_r13,
			   sleep_last_isr, sleep_last_cpu_wake, sleep_last_dvfs, sleep_last_fsm,
			   spm_rd(SPM_PWR_STATUS), spm_rd(SPM_PWR_STATUS_S));
	n += sysfs_emit_at(buf, n, "uart_busy=%u refused_pending=%u restore_failures=%u counter_before=%llu counter_after=%llu counter_ns=%llu pcm_ns=%llu injected_ns=%llu injections=%u\n",
			   sleep_uart_busy, sleep_refused_pending, sleep_restore_failures, sleep_counter_before,
			   sleep_counter_after, sleep_counter_ns,
			   sleep_pcm_ns, sleep_injected_ns, sleep_injections);
	return n + sysfs_emit_at(buf, n, "suspend_pcm_words=%u suspend_pcm_origin=MT6572_c4b775c_pcm_suspend_v26-42 system_sleep_abi=%u maintenance_seconds=%u timer_wake=%u\n",
				 Y1_SUSPEND_PCM_WORDS, READ_ONCE(sleep_ready) && READ_ONCE(sleep_enabled),
				 maintenance, timer_wake);
}
static DEVICE_ATTR_RO(state);
static int __init y1_sleep_init(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "mediatek,mt6572-scpsys");
	struct platform_device *pdev = np ? of_find_device_by_node(np) : NULL;
	int ret;
	of_node_put(np);
	if (!pdev) {
		sleep_stage = "no_scpsys";
		return -ENODEV;
	}
	ret = device_create_file(&pdev->dev, &dev_attr_state);
	put_device(&pdev->dev);
	if (ret) {
		sleep_stage = "state_attr";
		return ret;
	}
	__cpuc_flush_dcache_area((void *)y1_suspend_pcm, sizeof(y1_suspend_pcm));
	sp_pcm_phys = __pa_symbol(y1_suspend_pcm);
	register_syscore(&sleep_syscore);
	suspend_set_ops(&sleep_ops);
	sleep_stage = "ready";
	smp_store_release(&sleep_ready, true);
	return 0;
}
#else
static int __init y1_sleep_init(void)
{
	return 0;
}
static const char *sleep_stage = "off";
#endif
static void __init dp_unmap(void)
{
	void __iomem **maps[] = {
		&dp_spm, &dp_top, &dp_mm, &dp_gpt, &dp_cirq, &dp_dist, &dp_pol, &dp_boot, &dp_cache,
	};
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
	dp_boot = ioremap(BOOT_SLV_BASE, 8);
	dp_cache = ioremap(CA7_CACHE_CONFIG, 4);
	if (!dp_spm || !dp_top || !dp_mm || !dp_gpt || !dp_cirq || !dp_dist || !dp_pol || !dp_boot ||
	    !dp_cache) {
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
	if (y1_sleep_init())
		pr_err("y1-deep-idle: system sleep not available: %s\n", sleep_stage);
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
