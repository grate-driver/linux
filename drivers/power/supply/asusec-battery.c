/*
 * ASUS EC driver - battery monitoring
 *
 * Written by: Michał Mirosław <mirq-linux@rere.qmqm.pl>
 *
 * Copyright (C) 2017 Michał Mirosław
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/mfd/asusec.h>

#define ASUSEC_BATTERY_DATA_FRESH	(HZ / 20)

#define TEMP_CELSIUS_OFFSET		2731

#define ASUSEC_CTL_TEST_DISCHARGE	(1ull << 0x23)		// 4.3

#define ASUSEC_BATTERY_CHARGING		0x40
#define ASUSEC_BATTERY_FULL_CHARGED	0x20
#define ASUSEC_BATTERY_FULL_DISCHARGED	0x10

struct asusec_battery_data {
	const struct asusec_info *ec;
	struct power_supply	*battery;
	struct mutex		 data_lock;
	unsigned long		 batt_data_ts;
	u8			 batt_data[DOCKRAM_ENTRY_BUFSIZE];
};

static int asusec_battery_refresh(struct asusec_battery_data *priv)
{
	int ret = 0;

	mutex_lock(&priv->data_lock);

	if (time_before(jiffies, priv->batt_data_ts))
		goto out_unlock;

	// if (sleeping)	FIXME: runtime_pm?
	asusec_signal_request(priv->ec);

	ret = asus_dockram_read(priv->ec->dockram, 0x14, priv->batt_data);
	if (ret < 0)
		goto out_unlock;

	priv->batt_data_ts = jiffies + ASUSEC_BATTERY_DATA_FRESH;

out_unlock:
	mutex_unlock(&priv->data_lock);
	return ret;
}

__attribute__((unused))
static int asusec_battery_test_discharge(struct asusec_battery_data *priv)
{
	return asusec_set_ctl_bits(priv->ec, ASUSEC_CTL_TEST_DISCHARGE);
}

static enum power_supply_property pad_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
};

static const unsigned pad_battery_prop_offs[] = {
	[POWER_SUPPLY_PROP_STATUS] = 1,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = 3,
	[POWER_SUPPLY_PROP_TEMP] = 7,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = 9,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = 11,
	[POWER_SUPPLY_PROP_CAPACITY] = 13,
	[POWER_SUPPLY_PROP_CHARGE_NOW] = 15,
	[POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG] = 17,
	[POWER_SUPPLY_PROP_TIME_TO_FULL_AVG] = 19,
};

static int pad_battery_get_value(struct asusec_battery_data *priv,
				 enum power_supply_property psp)
{
	int ret, offs;

	if (psp >= ARRAY_SIZE(pad_battery_prop_offs))
		return -EINVAL;
	if (!pad_battery_prop_offs[psp])
		return -EINVAL;

	ret = asusec_battery_refresh(priv);
	if (ret < 0)
		return ret;

	offs = pad_battery_prop_offs[psp];
	return priv->batt_data[offs + 1] << 8 | priv->batt_data[offs];
}

static int pad_battery_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct asusec_battery_data *priv = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		if (of_property_read_s32(psy->of_node, "charge-full-design", &val->intval))
			return -ENODATA;
		break;

	/* POWER_SUPPLY_PROP_VOLTAGE_? nominal: 7.5V */

	default:
		ret = pad_battery_get_value(priv, psp);
		if (ret < 0)
			return ret;

		val->intval = (s16)ret;

		if (psp == POWER_SUPPLY_PROP_STATUS) {
			if (ret & ASUSEC_BATTERY_FULL_CHARGED)
				val->intval = POWER_SUPPLY_STATUS_FULL;
			else if (ret & ASUSEC_BATTERY_FULL_DISCHARGED)
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			else if (ret & ASUSEC_BATTERY_CHARGING)
				val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			else
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
		} else if (psp == POWER_SUPPLY_PROP_TEMP) {
			val->intval -= TEMP_CELSIUS_OFFSET;
		}

		break;
	}

	return 0;
}

static const struct power_supply_desc pad_battery_desc = {
	.name = "pad_battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = pad_battery_properties,
	.num_properties = ARRAY_SIZE(pad_battery_properties),
	.get_property = pad_battery_get_property,
};

static const struct power_supply_desc dock_battery_desc = {
	.name = "dock_battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = pad_battery_properties,
	.num_properties = ARRAY_SIZE(pad_battery_properties),
	.get_property = pad_battery_get_property,
};

int asusec_battery_probe(struct platform_device *dev)
{
	const struct power_supply_desc *psd;
	struct asusec_battery_data *priv;
	struct power_supply_config cfg = { 0, };
	const struct asusec_info *ec = asusec_cell_to_ec(dev);

	priv = devm_kzalloc(&dev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(dev, priv);
	priv->ec = ec;

	cfg.of_node = of_get_child_by_name(dev->dev.parent->of_node, "battery");
	cfg.drv_data = priv;

	mutex_init(&priv->data_lock);
	priv->batt_data_ts = jiffies - 1;

	if (of_property_read_bool(cfg.of_node, "non-removable"))
		psd = &pad_battery_desc;
	else
		psd = &dock_battery_desc;

	priv->battery = devm_power_supply_register(&dev->dev, psd, &cfg);
	if (IS_ERR(priv->battery))
		return PTR_ERR(priv->battery);

	return 0;
}

static struct platform_driver asusec_battery_driver = {
	.driver.name = "asusec-battery",
	.probe = asusec_battery_probe,
};

module_platform_driver(asusec_battery_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer Pad battery driver");
MODULE_LICENSE("GPL");
