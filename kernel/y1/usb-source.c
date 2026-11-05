// SPDX-License-Identifier: GPL-2.0-only
/*
 * Innioasis Y1: the Y2 charger's USB contract (kernel/platform/source.h) on mainline MUSB
 * (docs/planning/Y1-CHARGING-PLAN.md, stage B). The Y1 runs no BC1.1: its charger uses
 * innioasis,usb-budget-source, so begin/end are never reached and refuse if they are.
 *
 * "Configured by a PC" needs two signals. The composite layer reports its budget through
 * usb_gadget_vbus_draw, which MUSB passes to usb_phy_set_power on the glue's generic transceiver (it has
 * no set_power of its own; this driver provides it): 100 mA after a bus reset, 2 mA in suspend, 0 on
 * disconnect, and on configuration the configuration's MaxPower, which is 0 for g_serial (self-powered,
 * legacy/serial.c) and would be CONFIG_USB_GADGET_VBUS_DRAW otherwise. MUSB sets the same transceiver's
 * OTG state to B_PERIPHERAL on a bus reset from a host and to B_IDLE on disconnect (musb_gadget.c).
 * Configured = B_PERIPHERAL with a budget of 0 or at least 500 mA; a wall charger never resets the bus.
 * Only then is there an allocation; otherwise -1, the charger's floor (stage B run 1 in
 * docs/planning/Y1-CHARGING-PLAN.md found the self-powered 0 mA).
 */
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/usb/otg.h>
#include <linux/usb/phy.h>
#include "source.h"

#define Y1_CONFIGURED_MA 500

static atomic_t y1_budget_ma = ATOMIC_INIT(0);
static struct power_supply *y1_input;
static struct usb_phy *y1_phy;

static bool y1_usb_configured(void)
{
	struct usb_phy *phy = READ_ONCE(y1_phy);
	int ma = atomic_read(&y1_budget_ma);
	if (!phy || !phy->otg || READ_ONCE(phy->otg->state) != OTG_STATE_B_PERIPHERAL) return false;
	return ma == 0 || ma >= Y1_CONFIGURED_MA;
}

int y2_usb_charge_allocation(void)
{
	return y1_usb_configured() ? Y1_CONFIGURED_MA * 1000 : -1;
}

int y2_usb_bc11_begin(void)
{
	return -ENODEV;
}

int y2_usb_bc11_end(bool data_source)
{
	return 0;
}

void y2_usb_source_invalidate(void)
{
}

static int y1_usb_set_power(struct usb_phy *phy, unsigned int ma)
{
	struct power_supply *input = READ_ONCE(y1_input);
	if (atomic_xchg(&y1_budget_ma, ma) != (int)ma && input)
		power_supply_changed(input);
	return 0;
}

static enum power_supply_property y1_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_CURRENT_MAX,
};

static int y1_usb_get(struct power_supply *psy, enum power_supply_property prop,
		      union power_supply_propval *val)
{
	bool configured = y1_usb_configured();
	switch (prop) {
	case POWER_SUPPLY_PROP_ONLINE: val->intval = configured; return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX: val->intval = configured ? Y1_CONFIGURED_MA * 1000 : 0; return 0;
	default: return -EINVAL;
	}
}

static const struct power_supply_desc y1_usb_desc = {
	.name = "y1-usb-input", .type = POWER_SUPPLY_TYPE_USB,
	.properties = y1_usb_props, .num_properties = ARRAY_SIZE(y1_usb_props), .get_property = y1_usb_get,
};

static int y1_usb_source_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = { .fwnode = dev_fwnode(dev) };
	struct power_supply *input;
	struct usb_phy *phy;

	/* The MUSB glue registers this transceiver; until it has, wait. */
	phy = devm_usb_get_phy(dev, USB_PHY_TYPE_USB2);
	if (IS_ERR(phy)) return -EPROBE_DEFER;
	if (phy->set_power) return dev_err_probe(dev, -EBUSY, "transceiver already reports power\n");
	input = devm_power_supply_register(dev, &y1_usb_desc, &cfg);
	if (IS_ERR(input)) return PTR_ERR(input);
	WRITE_ONCE(y1_input, input);
	WRITE_ONCE(y1_phy, phy);
	phy->set_power = y1_usb_set_power;
	dev_info(dev, "USB budget source: %u mA once a PC configured the gadget, charger floor otherwise\n",
		 Y1_CONFIGURED_MA);
	return 0;
}

static const struct of_device_id y1_usb_source_match[] = { { .compatible = "innioasis,y1-usb-input" }, {} };
static struct platform_driver y1_usb_source_driver = {
	.probe = y1_usb_source_probe,
	.driver = { .name = "y1-usb-input", .of_match_table = y1_usb_source_match, .suppress_bind_attrs = true },
};
module_platform_driver(y1_usb_source_driver);
MODULE_DESCRIPTION("Innioasis Y1 USB budget source for the MT6323 charger");
MODULE_LICENSE("GPL");
