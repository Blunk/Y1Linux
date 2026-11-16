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
 * engine. The path's own driverless clocks (SMI common, LARB0, BLS) now follow OVL (patches-y1 0019). */
#include <dt-bindings/clock/mediatek,mt6572-clk.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
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
static int __init y1_deep_idle_init(void)
{
	if (!of_machine_is_compatible("innioasis,y1"))
		return -ENODEV;
	y1_deep_idle_release_unused();
	schedule_delayed_work(&slots_work, 0);
	return 0;
}
late_initcall(y1_deep_idle_init);
MODULE_LICENSE("GPL");
