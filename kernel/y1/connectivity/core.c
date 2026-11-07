// SPDX-License-Identifier: GPL-2.0-only
/* Innioasis Y1 (MT6572) CONSYS owner, Bluetooth only (docs/planning/Y1-BLUETOOTH-PLAN.md B2):
 * the Y2's kernel/platform/connectivity/core.c lifecycle (runtime PM, function references,
 * bounded recovery, root-only factory hand-off and activation) with the MT6572 power-on decoded
 * from stock in B1 (out/y1-bluetooth-b1-notes.md §1, §6). Separate from the Y2's core.c because
 * almost every power step differs: no modem calibration, no RGU reset, no EMI remap, no CONNMCU
 * clock, VCN28 on (TCXO path), the SPM clock, the CONN domain sequence of y1-conn-policy.h, the
 * MCU running from RST_B before BTIF opens, and a sleep handshake before power-off. Wi-Fi
 * (docs/planning/Y1-WIFI-PLAN.md W2): the Y2's fullmac as a child device, VCN33_WIFI in hardware mode
 * while it runs (MediaTek's MT6572 HifAhbProbe) and no WMT function command (its wmt_func_wifi_on). */
#include <crypto/sha2.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include "conn.h"

/* As the Y2: rails 2 and 3 are the PA LDOs with their ON_CTRL bits (the same MT6323 bits as
 * stock MT6572). VCN28's hardware mode is switched in power_on/power_off, in stock's order. */
int y2_conn_rail(struct y2_conn *c, unsigned rail, bool enable)
{
	static const unsigned reg[] = {0,0,0x416,0x418};
	static const unsigned hw[] = {0,0,BIT(5),BIT(14)};
	int ret = 0;
	if (rail >= ARRAY_SIZE(c->rails)) return -EINVAL;
	if (enable && !c->rail_on[rail]) {
		ret = regulator_enable(c->rails[rail].consumer);
		if (ret) return ret;
		c->rail_on[rail] = true;
	}
	if (hw[rail]) ret = regmap_update_bits(c->pmic,reg[rail],hw[rail],enable ? hw[rail] : 0);
	if (!enable && !ret && c->rail_on[rail]) {
		ret = regulator_disable(c->rails[rail].consumer);
		if (!ret) c->rail_on[rail] = false;
	}
	return ret;
}
static int pmic_prepare(struct y2_conn *c)
{
	/* Stock MT6572 PMIC_INIT_SETTING_V1 (0xc03da084) carries the Y2's connectivity subset (B1 §1.3);
	 * LK leaves it unapplied (B4 baseline). Stock applies it once in its PMIC init (call 0xc03da760)
	 * and goes straight on (PMIC_EINT_SETTING at 0xc03da774): no settle. Probe applies it here the
	 * same way, long before the first power-on; the Y2's 5 s wait is not carried over (B5 fix). */
	static const unsigned fields[][3] = {
		{0x102,BIT(6),BIT(6)}, {0x102,BIT(11),0}, {0x102,BIT(15),BIT(15)},
		{0x120,BIT(4),BIT(4)}, {0x120,BIT(5),BIT(5)},
		{0x148,BIT(1),BIT(1)}, {0x148,BIT(3),BIT(3)},
		{0x402,BIT(0),BIT(0)}, {0x402,BIT(11),0},
	};
	int ret;
	if (c->pmic_ready) return 0;
	for (unsigned i=0;i<ARRAY_SIZE(fields);i++) {
		ret=regmap_update_bits(c->pmic,fields[i][0],fields[i][1],fields[i][2]);
		if (ret) return ret;
	}
	c->pmic_ready = true;
	return 0;
}
static int step(struct y2_conn *c, const struct y1_conn_step *s)
{
	struct regmap *map = s->op >= Y1_INFRA_SET && s->op <= Y1_INFRA_POLL_CLEAR ? c->infracfg : c->spm;
	unsigned value;
	switch (s->op) {
	case Y1_SPM_WRITE: return regmap_write(map, s->reg, s->value);
	case Y1_SPM_SET: case Y1_INFRA_SET: return regmap_set_bits(map, s->reg, s->value);
	case Y1_SPM_CLEAR: case Y1_INFRA_CLEAR: return regmap_clear_bits(map, s->reg, s->value);
	case Y1_SPM_POLL_SET: case Y1_INFRA_POLL_SET:
		return regmap_read_poll_timeout(map, s->reg, value, (value & s->value) == s->value,
						10, 100000);
	case Y1_SPM_POLL_CLEAR: case Y1_INFRA_POLL_CLEAR:
		return regmap_read_poll_timeout(map, s->reg, value, !(value & s->value), 10, 100000);
	case Y1_DELAY_US: udelay(s->value); return 0;
	}
	return -EINVAL;
}
/* Stock acknowledges with a WARN and goes on; here a missing acknowledgement stops the
 * sequence and the caller isolates (the Y2's rule: live-domain timeouts propagate). */
static int domain_on(struct y2_conn *c)
{
	for (unsigned i = 0; i < ARRAY_SIZE(y1_conn_domain_on); i++) {
		int ret = step(c, y1_conn_domain_on + i);
		if (ret) {
			dev_err(c->dev, "CONN on step %u (reg %#x) failed: %d\n",
				i, y1_conn_domain_on[i].reg, ret);
			return ret;
		}
	}
	return 0;
}
/* Every step even after a missing acknowledgement, as stock (WARN and go on): isolation and the
 * switches off are better than a half-on domain. The first error is still reported. */
/* Stock spm_mtcmos_ctrl_general_locked (0xc00407c4-0xc004080c): protection set, the 0x228
 * acknowledgement polled until jiffies+10, and on a timeout a WARN (0xc0040b28) and the isolation
 * anyway. After the sleep handshake the sleeping core does not acknowledge (B5 run: 0x228 stays
 * 0xf000; B4, never asleep: 0xf011). The core asleep (SPM 0x100063b4 bit 11 clear) and BTIF/DMA
 * stopped are the idle guarantee there, so that timeout is logged, not an error. Without the
 * handshake a missing acknowledgement stays an error (the isolation still goes on, as stock). */
static int domain_off(struct y2_conn *c, bool asleep)
{
	int first = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(y1_conn_domain_off); i++) {
		const struct y1_conn_step *s = y1_conn_domain_off + i;
		int ret = step(c, s);
		if (s->op == Y1_INFRA_POLL_SET) {
			unsigned ack = 0;
			regmap_read(c->infracfg, Y1_INFRA_BUS_PROT_STA, &ack);
			dev_info(c->dev, "CONN bus protection: 0x228=%#x, %s%s\n", ack,
				 ret ? "not acknowledged" : "acknowledged",
				 ret && asleep ? " (core asleep: continuing, as stock)" : "");
			if (ret && asleep) ret = 0;
		}
		if (ret) {
			dev_err(c->dev, "CONN off step %u (reg %#x) failed: %d\n",
				i, y1_conn_domain_off[i].reg, ret);
			if (!first) first = ret;
		}
	}
	return first;
}
/* Stock waits up to 10 s for the radio core to sleep before conn_power_off; a core that never
 * sleeps is still isolated, as stock does after its wait. */
static bool sleep_wait(struct y2_conn *c)
{
	unsigned value;
	if (regmap_read_poll_timeout(c->spm, Y1_SPM_SLEEP_STATUS, value, !(value & Y1_SPM_CONN_AWAKE),
				     100000, 10000000)) {
		dev_warn(c->dev, "CONN did not sleep within 10 s; isolating\n");
		return false;
	}
	return true;
}
static int rails_off(struct y2_conn *c)
{
	int ret = 0, result;
	/* The PA rails only if acquired: the chip-id bring-up never touches them. */
	for (int i = 3; i >= 2; i--) {
		if (!c->rails[i].consumer) continue;
		result = y2_conn_rail(c, i, false); if (!ret) ret = result;
	}
	result = regmap_update_bits(c->pmic, Y1_PMIC_VCN18_CON, Y1_PMIC_VCN18_LP, 0);
	if (!ret) ret = result;
	result = y2_conn_rail(c, 0, false); if (!ret) ret = result;
	result = regmap_update_bits(c->pmic, Y1_PMIC_VCN28_CON, Y1_PMIC_VCN28_HW_MODE, 0);
	if (!ret) ret = result;
	result = y2_conn_rail(c, 1, false); if (!ret) ret = result;
	return ret;
}
static int power_off(struct y2_conn *c)
{
	int ret, result, stop_error, domain_error = 0;
	if (!c->powered && !c->dma_active &&
	    !c->rail_on[0] && !c->rail_on[1] && !c->rail_on[2] && !c->rail_on[3]) return 0;
	stop_error = y2_btif_stop(c);
	y2_stp_reset(c);
	if (c->powered) {
		domain_error = domain_off(c, sleep_wait(c));
		/* The DMA channel reset needs both power-off acknowledgements (y2_btif_abort). */
		if (domain_error && c->dma_active) return domain_error;
		c->powered = false;
	}
	if (c->dma_active) {
		/* As the Y2: after isolation only, and it resolves the earlier STOP error. */
		result = y2_btif_abort(c);
		if (result) return result;
		stop_error = 0;
	}
	ret = rails_off(c);
	return domain_error ? domain_error : stop_error ? stop_error : ret;
}
/* LK leaves CONN off (B4 baseline: 0x10006280 = 0x1112, PWR_STATUS bit 1 clear). Should it be on,
 * take it down first, without the sleep wait: nothing of ours runs there yet. */
static int normalise(struct y2_conn *c)
{
	unsigned control, status;
	int ret = regmap_read(c->spm, Y1_SPM_CONN_PWR_CON, &control);
	if (!ret) ret = regmap_read(c->spm, Y1_SPM_PWR_STATUS, &status);
	if (ret) return ret;
	if (!(control & (Y1_PWR_ON | Y1_PWR_ON_2ND | Y1_PWR_RST_B)) && !(status & Y1_SPM_CONN_STATUS))
		return 0;
	dev_warn(c->dev, "CONN found on (%#x); powering it off first\n", control);
	return domain_off(c, false);
}
/* Stock order per rail: VCN_1V8 out of low power, then on; VCN28 under SRCLKEN, then on. */
static int vcn18_on(struct y2_conn *c)
{
	int ret = regmap_update_bits(c->pmic, Y1_PMIC_VCN18_CON, Y1_PMIC_VCN18_LP, 0);
	return ret ? ret : y2_conn_rail(c, 0, true);
}
static int vcn28_on(struct y2_conn *c)
{
	int ret = regmap_update_bits(c->pmic, Y1_PMIC_VCN28_CON, Y1_PMIC_VCN28_HW_MODE,
				     Y1_PMIC_VCN28_HW_MODE);
	if (!ret) ret = y2_conn_rail(c, 1, true);
	if (!ret) usleep_range(150, 250); /* the Y2's settle before the domain; stock has none */
	return ret;
}
static int spm_clock(struct y2_conn *c)
{
	/* TOPCKGEN CG_CTRL1 bit 26 also serves the SPM itself: never gated again (stock). */
	int ret = c->clock_on ? 0 : clk_prepare_enable(c->clocks[0].clk);
	if (!ret) c->clock_on = true;
	return ret;
}
/* 0x18070008 is read only here: after DOMAIN_ON has released RST_B and the bus protection. */
static int chip_id(struct y2_conn *c)
{
	unsigned id;
	int ret;
	udelay(10);
	ret = readl_poll_timeout(c->mcu + 8, id, (id & 0xffff) == Y1_CONN_CHIP, 20000, 200000);
	c->chip = id & 0xffff;
	if (ret) dev_err(c->dev, "CONN chip ID absent: %#010x after 200 ms\n", id);
	else dev_info(c->dev, "CONN chip=%04x (register %#010x)\n", c->chip, id);
	return ret;
}
static int wmt(struct y2_conn *c)
{
	msleep(5);
	WRITE_ONCE(c->failure,0);
	return y1_wmt_boot(c);
}
static int stage(struct y2_conn *c, enum y1_conn_stage s)
{
	switch (s) {
	case Y1_STAGE_NORMALISE: return normalise(c);
	case Y1_STAGE_VCN18: return vcn18_on(c);
	case Y1_STAGE_VCN28: return vcn28_on(c);
	case Y1_STAGE_EMI_CLEAR: memset_io(c->emi, 0, Y1_CONN_EMI_SIZE); return 0;
	case Y1_STAGE_SPM_CLOCK: return spm_clock(c);
	/* From here the domain may be partly on: power_off must run its sequence. */
	case Y1_STAGE_DOMAIN_ON: c->powered = true; return domain_on(c);
	case Y1_STAGE_CHIP_ID: return chip_id(c);
	case Y1_STAGE_BTIF: return y2_btif_start(c);
	case Y1_STAGE_WMT: return wmt(c);
	case Y1_STAGE_OFF: return power_off(c);
	}
	return -EINVAL;
}
/* Runs the stages; on a failure powers off and reports the first error. */
static int stages(struct y2_conn *c, const unsigned char *list, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		/* A bring-up abort (power_off attribute) ends the attempt between stages. */
		int ret = READ_ONCE(c->abort) ? -ECANCELED : 0;
		if (!ret && c->mode != Y1_MODE_FULL)
			dev_info(c->dev, "stage %s\n", y1_conn_stage_names[list[i]]);
		if (!ret) ret = stage(c, list[i]);
		if (ret) {
			int stopped = list[i] == Y1_STAGE_OFF ? 0 : power_off(c);
			dev_err(c->dev, "connectivity stage %s failed: %d (power-off %d)\n",
				y1_conn_stage_names[list[i]], ret, stopped);
			return ret;
		}
	}
	return 0;
}
static int power_on_once(struct y2_conn *c)
{
	int ret;
	if (!c->factory_ready || c->removing) return -EHOSTDOWN;
	ret = pmic_prepare(c);
	if (ret) return ret;
	if (READ_ONCE(c->removing)) return -ESHUTDOWN;
	return stages(c, y1_conn_full_stages, ARRAY_SIZE(y1_conn_full_stages));
}
/* The Y2's bounded retry (its mandatory-STP opcode 08 receipt): once, only fully isolated. */
static unsigned boot_retries;
static int power_on(struct y2_conn *c)
{
	int ret = power_on_once(c);
	if ((ret == -ETIMEDOUT || ret == -EIO) && !c->powered && !c->dma_active &&
	    !c->rail_on[0] && !c->rail_on[1] && !c->rail_on[2] && !c->rail_on[3]) {
		boot_retries++;
		dev_warn(c->dev, "controller boot failed %d; isolated, retrying once\n", ret);
		msleep(20);
		ret = power_on_once(c);
	}
	return ret;
}
static int runtime_resume(struct device *dev)
{
	struct y2_conn *c=dev_get_drvdata(dev); int ret;
	mutex_lock(&c->lifecycle);
	ret=power_on(c);
	if (ret) WRITE_ONCE(c->failure,ret);
	mutex_unlock(&c->lifecycle);
	return ret;
}
/* Before a regular power-off: the radio core to sleep (WMT sleep; B5 run 2: it then sleeps at once and
 * the power-off needs no 10 s wait). Only over a healthy, booted transport; otherwise the power-off
 * isolates without it (sleep wait, then the stock rule for the protection acknowledgement). */
static int sleep_then_off(struct y2_conn *c)
{
	if (c->powered && c->dma_active && c->full_stp && !READ_ONCE(c->failure)) {
		int ret = y1_wmt_sleep(c);
		if (ret) dev_warn(c->dev, "WMT sleep before power-off: %d\n", ret);
	}
	return power_off(c);
}
static int runtime_suspend(struct device *dev)
{
	struct y2_conn *c=dev_get_drvdata(dev); int ret;
	mutex_lock(&c->lifecycle);
	ret=c->functions ? -EBUSY : sleep_then_off(c);
	mutex_unlock(&c->lifecycle);
	return ret;
}
/* The PA rail of a function: VCN33_BT (rail 2) or VCN33_WIFI (rail 3), each with its ON_CTRL bit. */
static unsigned function_rail(unsigned function)
{
	return function==Y2_CONN_BT ? 2 : 3;
}
/* MediaTek's MT6572 switches Wi-Fi by its probe alone (y1-conn-policy.h); Bluetooth by WMT. */
static int function_switch(struct y2_conn *c, unsigned function, bool enable)
{
	if (function==Y2_CONN_WIFI && !Y1_CONN_WIFI_WMT_FUNCTION) return 0;
	return y2_wmt_function(c,function,enable);
}
int y2_conn_get(struct y2_conn *c, unsigned function)
{
	int ret;
	if (function!=Y2_CONN_BT && function!=Y2_CONN_WIFI) return -EINVAL;
	if (!READ_ONCE(c->activated) || READ_ONCE(c->recovering) || c->removing) return -EHOSTDOWN;
	ret=pm_runtime_resume_and_get(c->dev);
	if (ret<0) { y2_conn_failed(c,ret); return ret; }
	mutex_lock(&c->lifecycle);
	if (!c->powered && !c->failure) {
		ret=power_on(c);
		if (ret) { WRITE_ONCE(c->failure,ret); goto unused; }
	}
	if (c->functions & BIT(function)) { ret=0; goto unused; }
	ret=c->failure ? c->failure : y2_conn_rail(c,function_rail(function),true);
	if (!ret) ret=function_switch(c,function,true);
	if (ret) { y2_conn_rail(c,function_rail(function),false); goto unused; }
	c->functions |= BIT(function); c->wanted |= BIT(function);
	mutex_unlock(&c->lifecycle);
	return 0;
unused:
	mutex_unlock(&c->lifecycle); pm_runtime_put(c->dev);
	return ret;
}
int y2_conn_put(struct y2_conn *c, unsigned function)
{
	int ret=0, rail;
	if (function!=Y2_CONN_BT && function!=Y2_CONN_WIFI) return -EINVAL;
	mutex_lock(&c->lifecycle);
	c->wanted &= ~BIT(function);
	if (!(c->functions & BIT(function))) { mutex_unlock(&c->lifecycle); return 0; }
	if (!c->failure && c->powered) ret=function_switch(c,function,false);
	rail=y2_conn_rail(c,function_rail(function),false);
	if (!ret) ret=rail;
	c->functions &= ~BIT(function);
	mutex_unlock(&c->lifecycle);
	pm_runtime_mark_last_busy(c->dev); pm_runtime_put_autosuspend(c->dev);
	if (ret) y2_conn_failed(c,ret);
	return ret;
}
static int recover_locked(struct y2_conn *c)
{
	int ret;
	mutex_lock(&c->lifecycle);
	if (!c->activated || c->removing || c->recovering) { mutex_unlock(&c->lifecycle); return -EHOSTDOWN; }
	if (time_after(jiffies,c->last_recovery+msecs_to_jiffies(300000))) c->automatic_recoveries=0;
	if (c->automatic_recoveries++ >= 3) { mutex_unlock(&c->lifecycle); return -EIO; }
	c->last_recovery=jiffies; c->recovering=true;
	WRITE_ONCE(c->hci_paused,true);
	mutex_unlock(&c->lifecycle);
	/* As the Y2: quiesce consumers outside the lifecycle lock (their remove/close callbacks release
	 * function references through this owner); the Wi-Fi driver is bound again afterwards. */
	if (c->wifi) device_release_driver(&c->wifi->dev);
	if (c->hdev) cancel_work_sync(&c->hci_work);
	pm_runtime_disable(c->dev);
	mutex_lock(&c->lifecycle);
	ret=power_off(c);
	if (!ret) { WRITE_ONCE(c->failure,0); c->recoveries++; }
	if (!ret) pm_runtime_set_suspended(c->dev);
	pm_runtime_enable(c->dev);
	c->recovering=false;
	mutex_unlock(&c->lifecycle);
	if (!ret && c->wifi) {
		int attached=device_attach(&c->wifi->dev);
		if (attached<0) dev_warn(c->dev,"Wi-Fi recovery rebind: %d\n",attached);
	}
	return ret;
}
static void recover(struct work_struct *work)
{
	struct y2_conn *c=container_of(work,struct y2_conn,recovery_work);
	mutex_lock(&c->recovery_lock);
	if (!recover_locked(c) && c->hdev && c->hci_open) {
		c->hci_recovery_pending=true;
		if (hci_reset_dev(c->hdev)) c->hci_recovery_pending=false;
	}
	mutex_unlock(&c->recovery_lock);
}
void y2_conn_hci_error(struct y2_conn *c)
{
	mutex_lock(&c->recovery_lock);
	if (c->hci_recovery_pending) c->hci_recovery_pending=false;
	else {
		WRITE_ONCE(c->failure,-EIO); complete_all(&c->response); wake_up_all(&c->tx_wait);
		recover_locked(c);
	}
	mutex_unlock(&c->recovery_lock);
}
void y2_conn_failed(struct y2_conn *c, int error)
{
	if (error>=0) error=-EIO;
	if (!cmpxchg(&c->failure,0,error)) {
		c->transport_errors++;
		dev_crit(c->dev,"connectivity failure %d from %pS\n",error,__builtin_return_address(0));
	}
	complete_all(&c->response); wake_up_all(&c->tx_wait);
	if (READ_ONCE(c->activated) && !READ_ONCE(c->removing) && !READ_ONCE(c->recovering))
		schedule_work(&c->recovery_work);
}
/* The Y1 hand-off is the 32-byte BT_Addr record alone (no Wi-Fi record, no modem). The charge-only
 * mode never writes it, so no boot-mode gate is needed here (the Y2 checks its normal boot). */
static ssize_t factory_write(struct file *file, struct kobject *kobj, const struct bin_attribute *attr,
			    char *data, loff_t offset, size_t size)
{
	struct y2_conn *c=dev_get_drvdata(kobj_to_dev(kobj)); int ret=size;
	unsigned char digest[32];
	if (offset || size!=Y1_CONN_RECORD_SIZE) return -EINVAL;
	sha256((const u8 *)data, 6, digest);
	mutex_lock(&c->lifecycle);
	if (c->activated) ret=-EBUSY;
	else if (y1_conn_record_check((const u8 *)data, size, digest)) ret=-EINVAL;
	else { memcpy(c->bt_factory,data,Y1_CONN_RECORD_SIZE); c->factory_ready=true; }
	mutex_unlock(&c->lifecycle);
	memzero_explicit(digest, sizeof(digest));
	return ret;
}
static const struct bin_attribute bin_attr_factory = {
	.attr={.name="factory",.mode=0200}, .size=Y1_CONN_RECORD_SIZE, .write=factory_write,
};
/* The Wi-Fi record (y1-conn-policy.h): the Y1's stock default with a MAC, after the Bluetooth record and
 * before activation. Without it activation registers Bluetooth only. Once after activation too (the Wi-Fi
 * firmware imported later, W6): the record, then the Wi-Fi device, with no recovery running meanwhile. */
static int register_wifi(struct y2_conn *c);
static ssize_t wifi_factory_write(struct file *file, struct kobject *kobj, const struct bin_attribute *attr,
				  char *data, loff_t offset, size_t size)
{
	struct y2_conn *c=dev_get_drvdata(kobj_to_dev(kobj)); int ret=size;
	unsigned char digest[32], *copy;
	bool late=false;
	if (offset || size!=Y1_CONN_WIFI_RECORD_SIZE) return -EINVAL;
	copy=kmemdup(data,size,GFP_KERNEL);
	if (!copy) return -ENOMEM;
	memset(copy+Y1_CONN_WIFI_MAC,0,6);
	sha256(copy,size,digest);
	kfree(copy);
	mutex_lock(&c->recovery_lock);
	mutex_lock(&c->lifecycle);
	if (c->activated && (c->wifi_ready || c->wifi || c->removing || c->recovering)) ret=-EBUSY;
	else if (!c->factory_ready) ret=-ENODATA;
	else if (y1_conn_wifi_record_check((const u8 *)data,size,digest,c->bt_factory)) ret=-EINVAL;
	else { memcpy(c->wifi_factory,data,size); c->wifi_ready=true; late=c->activated; }
	mutex_unlock(&c->lifecycle);
	if (late) {
		int registered=register_wifi(c);
		if (registered) {
			mutex_lock(&c->lifecycle); c->wifi_ready=false; mutex_unlock(&c->lifecycle);
			ret=registered;
		} else if (c->wifi) dev_info(c->dev,"Wi-Fi registered after activation\n");
	}
	mutex_unlock(&c->recovery_lock);
	memzero_explicit(digest,sizeof(digest));
	return ret;
}
static const struct bin_attribute bin_attr_wifi_factory = {
	.attr={.name="wifi_factory",.mode=0200}, .size=Y1_CONN_WIFI_RECORD_SIZE, .write=wifi_factory_write,
};
/* The Y2's child device (wifi/ahb.c binds "y2-mt6582-wifi" and reads the parent's "wifi" window and
 * interrupt), only with the Wi-Fi build, the record and both resources in the node. */
static int register_wifi(struct y2_conn *c)
{
	struct platform_device *pdev=to_platform_device(c->dev);
	struct platform_device_info info={.parent=c->dev,.name="y2-mt6582-wifi",.id=PLATFORM_DEVID_NONE,
		.data=&c,.size_data=sizeof(c)};
	if (!IS_ENABLED(CONFIG_BT_INNIOASIS_Y1_WIFI) || !c->wifi_ready ||
	    !platform_get_resource_byname(pdev,IORESOURCE_MEM,"wifi")) return 0;
	c->wifi=platform_device_register_full(&info);
	if (IS_ERR(c->wifi)) { int ret=PTR_ERR(c->wifi); c->wifi=NULL; return ret; }
	return 0;
}
static ssize_t activate_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t n)
{
	struct y2_conn *c=dev_get_drvdata(dev); bool value; int ret;
	if (kstrtobool(buf,&value) || !value) return -EINVAL;
	/* recovery_lock: a late Wi-Fi record (wifi_factory_write) waits until the device is registered. */
	mutex_lock(&c->recovery_lock);
	mutex_lock(&c->lifecycle);
	if (!c->factory_ready) { mutex_unlock(&c->lifecycle); ret=-ENODATA; goto out; }
	if (c->activated) { mutex_unlock(&c->lifecycle); ret=n; goto out; }
	c->activated=true; mutex_unlock(&c->lifecycle);
	ret=y2_hci_register(c);
	if (ret) { c->activated=false; goto out; }
	ret=register_wifi(c);
	if (ret) { y2_hci_unregister(c); c->activated=false; goto out; }
	ret=n;
out:
	mutex_unlock(&c->recovery_lock);
	return ret;
}
static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct y2_conn *c=dev_get_drvdata(dev);
	return sysfs_emit(buf,"activated=%u powered=%u functions=%#x calibrated=%u chip=%04x hvr=%04x fvr=%04x error=%d transport_errors=%u recoveries=%u calibration=%u status=%04x\n",
		c->activated,c->powered,c->functions,c->calibrated,c->chip,c->hvr,c->fvr,
		c->failure,c->transport_errors,c->recoveries,c->calibration_size,c->calibration_status);
}
static ssize_t boot_retries_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(boot_retries));
}
static DEVICE_ATTR_RO(boot_retries);
static DEVICE_ATTR_WO(activate);
static DEVICE_ATTR_RO(status);
static ssize_t recover_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t n)
{
	struct y2_conn *c=dev_get_drvdata(dev); bool value;
	if (kstrtobool(buf,&value) || !value) return -EINVAL;
	mutex_lock(&c->lifecycle);
	if (c->functions || c->recovering || !c->activated || c->removing) {
		mutex_unlock(&c->lifecycle); return -EBUSY;
	}
	c->automatic_recoveries=0;
	mutex_unlock(&c->lifecycle);
	y2_conn_failed(c,-EIO);
	return n;
}
static DEVICE_ATTR_WO(recover);
static struct attribute *attrs[]={&dev_attr_activate.attr,&dev_attr_status.attr,&dev_attr_recover.attr,&dev_attr_boot_retries.attr,NULL};
static const struct bin_attribute *const bin_attrs[]={&bin_attr_factory,&bin_attr_wifi_factory,NULL};
static const struct attribute_group group={.attrs=attrs,.bin_attrs=bin_attrs};
static int suspend(struct device *dev)
{
	struct y2_conn *c=dev_get_drvdata(dev);
	if (READ_ONCE(c->functions) || READ_ONCE(c->recovering)) return -EBUSY;
	if (c->mode!=Y1_MODE_FULL) return READ_ONCE(c->powered) ? -EBUSY : 0;
	return pm_runtime_force_suspend(dev);
}
static const struct dev_pm_ops pm={
	SET_RUNTIME_PM_OPS(runtime_suspend,runtime_resume,NULL)
	.suspend=suspend,.resume=pm_runtime_force_resume,
	.freeze=suspend,.thaw=pm_runtime_force_resume,
	.poweroff=suspend,.restore=pm_runtime_force_resume,
};
static int validate_interrupts(struct device_node *node)
{
	for (unsigned i=0;i<ARRAY_SIZE(y1_conn_irqs);i++) {
		int at=of_property_match_string(node,"interrupt-names",y1_conn_irqs[i].name);
		u32 cell[3];
		if (at<0) return -EINVAL;
		for (unsigned j=0;j<3;j++)
			if (of_property_read_u32_index(node,"interrupts",at*3+j,cell+j)) return -EINVAL;
		if (cell[0]!=Y1_CONN_GIC_SPI || cell[1]!=y1_conn_irqs[i].spi || cell[2]!=Y1_CONN_LEVEL_LOW)
			return -EINVAL;
	}
	return 0;
}
/* The Wi-Fi window and interrupt: both or neither, and then exactly MediaTek's. */
static int validate_wifi(struct platform_device *pdev)
{
	struct resource *r=platform_get_resource_byname(pdev,IORESOURCE_MEM,y1_conn_wifi_range.name);
	int at=of_property_match_string(pdev->dev.of_node,"interrupt-names",y1_conn_wifi_irq.name);
	u32 cell[3];
	if (!r && at<0) return 0;
	if (!r || at<0 || r->start!=y1_conn_wifi_range.start || resource_size(r)!=y1_conn_wifi_range.size)
		return -EINVAL;
	for (unsigned j=0;j<3;j++)
		if (of_property_read_u32_index(pdev->dev.of_node,"interrupts",at*3+j,cell+j)) return -EINVAL;
	if (cell[0]!=Y1_CONN_GIC_SPI || cell[1]!=y1_conn_wifi_irq.spi || cell[2]!=Y1_CONN_LEVEL_LOW)
		return -EINVAL;
	return 0;
}
/* One fixed wiring (y1-conn-policy.h): reject a malformed node before any mapping or DMA. */
static int validate_resources(struct platform_device *pdev)
{
	struct device_node *node; struct resource memory; int ret;
	for (unsigned i=0;i<ARRAY_SIZE(y1_conn_ranges);i++) {
		struct resource *r=platform_get_resource_byname(pdev,IORESOURCE_MEM,y1_conn_ranges[i].name);
		if (!r || r->start!=y1_conn_ranges[i].start || resource_size(r)!=y1_conn_ranges[i].size)
			return -EINVAL;
	}
	if (validate_interrupts(pdev->dev.of_node) || validate_wifi(pdev)) return -EINVAL;
	node=of_parse_phandle(pdev->dev.of_node,"memory-region",0);
	if (!node) return -EINVAL;
	ret=of_address_to_resource(node,0,&memory);
	if (!of_property_read_bool(node,"no-map")) ret=-EINVAL;
	of_node_put(node);
	if (ret || memory.start!=Y1_CONN_EMI_START || resource_size(&memory)!=Y1_CONN_EMI_SIZE) return -EINVAL;
	return 0;
}
/* The chip-id bring-up takes only the SPM clock and VCN18/VCN28 (clocks[0], rails[0..1]). */
static int probe_suppliers(struct y2_conn *c, bool chip_id_only)
{
	struct device *dev=c->dev; struct device_node *node; struct platform_device *supplier; int ret;
	c->clocks[0].id="spm"; c->clocks[1].id="btif"; c->clocks[2].id="dma";
	ret=devm_clk_bulk_get(dev,chip_id_only ? 1 : 3,c->clocks); if (ret) return ret;
	c->rails[0].supply="vcn18"; c->rails[1].supply="vcn28";
	c->rails[2].supply="vcn33-bt"; c->rails[3].supply="vcn33-wifi";
	ret=devm_regulator_bulk_get(dev,chip_id_only ? 2 : 4,c->rails); if (ret) return ret;
	c->spm=syscon_regmap_lookup_by_phandle(dev->of_node,"innioasis,scpsys");
	if (IS_ERR(c->spm)) return PTR_ERR(c->spm);
	c->infracfg=syscon_regmap_lookup_by_phandle(dev->of_node,"mediatek,infracfg");
	if (IS_ERR(c->infracfg)) return PTR_ERR(c->infracfg);
	node=of_parse_phandle(dev->of_node,"innioasis,pwrap",0);
	if (!node) return -EINVAL;
	supplier=of_find_device_by_node(node); of_node_put(node);
	if (!supplier) return -EPROBE_DEFER;
	c->pmic=dev_get_regmap(&supplier->dev,NULL);
	put_device(&supplier->dev);
	return c->pmic ? 0 : -EPROBE_DEFER;
}
/* B4 (docs/planning/Y1-BLUETOOTH-PLAN.md): power on to the chip ID, log it, power off, and stay
 * unbound (-ENODEV: nothing held, nothing registered). A missing ID still powers off first. */
/* SPM and INFRA only (always clocked): never the radio core's or BTIF's windows. */
static void log_off_state(struct y2_conn *c, const char *mode)
{
	unsigned control, status, sleep, bus;
	if (!regmap_read(c->spm, Y1_SPM_CONN_PWR_CON, &control) &&
	    !regmap_read(c->spm, Y1_SPM_PWR_STATUS, &status) &&
	    !regmap_read(c->spm, Y1_SPM_SLEEP_STATUS, &sleep) &&
	    !regmap_read(c->infracfg, Y1_INFRA_BUS_PROT, &bus))
		dev_info(c->dev, "bring-up %s: off: ctl=%#x status=%#x sleep=%#x protect=%#x\n",
			 mode, control, status, sleep, bus);
}
static int bring_up_chip_id(struct y2_conn *c)
{
	int ret;
	dev_info(c->dev, "bring-up chip-id: CONN on, chip ID, CONN off\n");
	ret = stages(c, y1_conn_chip_id_stages, ARRAY_SIZE(y1_conn_chip_id_stages));
	log_off_state(c, "chip-id");
	if (ret) return ret;
	dev_info(c->dev, "bring-up chip-id: done, chip=%04x; HVR/FVR need WMT (B5)\n", c->chip);
	return -ENODEV;
}
/* The PMIC init subset (stock PMIC_INIT_SETTING_V1, the Y2's pmic_prepare), logged around it:
 * LK leaves it unapplied (B4 baseline 0x102=0003 0x120=0001 0x148=0004 0x402=8400). */
static int pmic_prepare_logged(struct y2_conn *c)
{
	static const unsigned regs[] = {0x102, 0x120, 0x148, 0x402};
	unsigned before[4], after[4];
	int ret;
	if (c->pmic_ready) return 0;
	for (unsigned i = 0; i < 4; i++) if (regmap_read(c->pmic, regs[i], before + i)) before[i] = ~0u;
	ret = pmic_prepare(c);
	for (unsigned i = 0; i < 4; i++) if (regmap_read(c->pmic, regs[i], after + i)) after[i] = ~0u;
	dev_info(c->dev, "PMIC init subset %d: 0x102 %04x->%04x 0x120 %04x->%04x 0x148 %04x->%04x 0x402 %04x->%04x\n",
		 ret, before[0], after[0], before[1], after[1], before[2], after[2], before[3], after[3]);
	return ret;
}
/* B5 (docs/planning/Y1-BLUETOOTH-PLAN.md): one attempt per write to bring_up. The full power-on
 * with BTIF and the WMT boot (patch, RF calibration, coexistence), then WMT sleep and the power-off;
 * no HCI device, no address, no modem. A failed stage has already powered off; the device stays bound
 * for the next attempt. */
static int bring_up_wmt(struct y2_conn *c)
{
	int ret;
	mutex_lock(&c->lifecycle);
	if (c->powered || c->dma_active) { mutex_unlock(&c->lifecycle); return -EBUSY; }
	WRITE_ONCE(c->abort, false); WRITE_ONCE(c->failure, 0);
	c->calibrated = false; c->calibration_size = c->calibration_status = 0;
	c->chip = c->hvr = c->fvr = 0;
	dev_info(c->dev, "bring-up wmt: attempt\n");
	ret = pmic_prepare_logged(c);
	if (!ret) ret = stages(c, y1_conn_full_stages, ARRAY_SIZE(y1_conn_full_stages));
	if (!ret) {
		int sleep = READ_ONCE(c->abort) ? -ECANCELED : y1_wmt_sleep(c);
		dev_info(c->dev, "bring-up wmt: WMT sleep %d\n", sleep);
		ret = stage(c, Y1_STAGE_OFF);
	}
	log_off_state(c, "wmt");
	dev_info(c->dev, "bring-up wmt: %s %d: chip=%04x HVR=%04x FVR=%04x calibration=%u bytes status=%04x\n",
		 ret ? "failed" : "done", ret, c->chip, c->hvr, c->fvr, c->calibration_size,
		 c->calibration_status);
	mutex_unlock(&c->lifecycle);
	return ret;
}
static ssize_t bring_up_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t n)
{
	struct y2_conn *c=dev_get_drvdata(dev); bool value; int ret;
	if (kstrtobool(buf,&value) || !value) return -EINVAL;
	ret=bring_up_wmt(c);
	return ret ? ret : n;
}
/* The abort: ends a running attempt (its WMT wait returns at once) and powers off. */
static ssize_t power_off_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t n)
{
	struct y2_conn *c=dev_get_drvdata(dev); bool value; int ret;
	if (kstrtobool(buf,&value) || !value) return -EINVAL;
	WRITE_ONCE(c->abort, true);
	y2_conn_failed(c, -ECANCELED);
	mutex_lock(&c->lifecycle);
	ret=power_off(c);
	mutex_unlock(&c->lifecycle);
	dev_info(dev, "bring-up wmt: power-off on request: %d\n", ret);
	return ret ? ret : n;
}
static DEVICE_ATTR_WO(bring_up);
static DEVICE_ATTR_WO(power_off);
static struct attribute *bring_up_attrs[]={&dev_attr_bring_up.attr,&dev_attr_power_off.attr,&dev_attr_status.attr,NULL};
static const struct attribute_group bring_up_group={.attrs=bring_up_attrs};
/* "innioasis,bring-up" limits the driver to an admitted stage; absent = the full driver. */
static int bring_up_mode(struct y2_conn *c)
{
	const char *name;
	int mode;
	c->mode = Y1_MODE_FULL;
	if (of_property_read_string(c->dev->of_node, "innioasis,bring-up", &name)) return 0;
	mode = y1_conn_mode(name);
	if (mode < 0) return -EINVAL;
	c->mode = mode;
	return 0;
}
static int probe(struct platform_device *pdev)
{
	struct device *dev=&pdev->dev;
	struct y2_conn *c=devm_kzalloc(dev,sizeof(*c),GFP_KERNEL);
	int ret;
	if (!c) return -ENOMEM;
	ret=validate_resources(pdev); if (ret) return ret;
	c->dev=dev;
	ret=bring_up_mode(c); if (ret) return ret;
	c->dev=dev; mutex_init(&c->lifecycle); mutex_init(&c->command);
	mutex_init(&c->recovery_lock);
	mutex_init(&c->dma_tx); mutex_init(&c->dma_rx); init_completion(&c->response);
	y2_stp_init(c); INIT_WORK(&c->recovery_work,recover);
	ret=probe_suppliers(c,c->mode==Y1_MODE_CHIP_ID); if (ret) return ret;
	c->mcu=devm_platform_ioremap_resource_byname(pdev,"mcu");
	c->emi=devm_platform_ioremap_resource_byname(pdev,"conn-emi");
	if (IS_ERR(c->mcu)) return PTR_ERR(c->mcu);
	if (IS_ERR(c->emi)) return PTR_ERR(c->emi);
	if (c->mode==Y1_MODE_CHIP_ID) return bring_up_chip_id(c);
	ret=pmic_prepare_logged(c); if (ret) return ret;
	/* BTIF/DMA: mapped, buffers and interrupts (not enabled) only; registers after their clocks. */
	ret=y2_btif_probe(pdev,c); if (ret) return ret;
	platform_set_drvdata(pdev,c);
	if (c->mode==Y1_MODE_WMT) {
		ret=devm_device_add_group(dev,&bring_up_group);
		if (!ret) dev_info(dev,"bring-up wmt: ready (bring_up runs one attempt, power_off aborts)\n");
		return ret;
	}
	pm_runtime_set_suspended(dev); pm_runtime_set_autosuspend_delay(dev,1000);
	pm_suspend_ignore_children(dev,true);
	pm_runtime_use_autosuspend(dev); pm_runtime_enable(dev);
	ret=devm_device_add_group(dev,&group);
	if (ret) { pm_runtime_disable(dev); return ret; }
	dev_info(dev,"MT6572 connectivity owner ready (Bluetooth%s); radio awaits the factory record\n",
		 IS_ENABLED(CONFIG_BT_INNIOASIS_Y1_WIFI) &&
		 platform_get_resource_byname(pdev,IORESOURCE_MEM,"wifi") ? ", Wi-Fi" : "");
	return 0;
}
static void shutdown(struct platform_device *pdev)
{
	struct y2_conn *c=platform_get_drvdata(pdev);
	c->removing=true;
	if (c->mode==Y1_MODE_WMT) { WRITE_ONCE(c->abort,true); y2_conn_failed(c,-ESHUTDOWN); }
	cancel_work_sync(&c->recovery_work);
	if (c->wifi) { platform_device_unregister(c->wifi); c->wifi=NULL; }
	y2_hci_unregister(c); pm_runtime_disable(c->dev);
	mutex_lock(&c->lifecycle); sleep_then_off(c); mutex_unlock(&c->lifecycle);
}
static const struct of_device_id match[]={ {.compatible="innioasis,y1-mt6572-connectivity"}, {} };
static struct platform_driver y1_conn_driver={ .probe=probe,.shutdown=shutdown,
	.driver={.name="y1-connectivity",.of_match_table=match,.pm=&pm,.suppress_bind_attrs=true,
		 /* The power-off's sleep wait may take 10 s: never in the boot's critical path. */
		 .probe_type=PROBE_PREFER_ASYNCHRONOUS} };
builtin_platform_driver(y1_conn_driver);
