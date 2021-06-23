// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASUS EC driver - battery monitoring
 *
 * Written by: Michał Mirosław <mirq-linux@rere.qmqm.pl>
 *
 * Copyright (C) 2017 Michał Mirosław
 * Copyright (C) 2021 Svyatoslav Ryhel
 *
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/mfd/asus-ec.h>

#define ASUSEC_BATTERY_DATA_FRESH_MSEC		5000

#define ASUSEC_BATTERY_DISCHARGING		0x40
#define ASUSEC_BATTERY_FULL_CHARGED		0x20
#define ASUSEC_BATTERY_FULL_DISCHARGED		0x10

#define ASUSEC_CHARGER_USB_MASK			0x43

#define TEMP_CELSIUS_OFFSET			2731

struct asusec_battery_data {
	const struct asusec_info		*ec;
	struct power_supply			*battery;
	struct power_supply_battery_info	batt_info;
	struct delayed_work			poll_work;
	struct mutex				battery_lock;
	unsigned int				battery_addr;
	unsigned int				charger_addr;
	unsigned long				batt_data_ts;
	int					last_state;
	u8					batt_data[DOCKRAM_ENTRY_BUFSIZE];
};

static int asusec_battery_refresh(struct asusec_battery_data *priv)
{
	int ret = 0;

	mutex_lock(&priv->battery_lock);

	if (time_before(jiffies, priv->batt_data_ts))
		goto out_unlock;

	ret = asus_dockram_read(priv->ec->dockram, priv->battery_addr,
				priv->batt_data);
	if (ret < 0)
		goto out_unlock;

	priv->batt_data_ts = jiffies + msecs_to_jiffies(ASUSEC_BATTERY_DATA_FRESH_MSEC);

out_unlock:
	mutex_unlock(&priv->battery_lock);

	return ret;
}

static enum power_supply_property pad_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_TEMP_MIN,
	POWER_SUPPLY_PROP_TEMP_MAX,
};

static const unsigned int pad_battery_prop_offs[] = {
	[POWER_SUPPLY_PROP_STATUS] = 1,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = 3,
	[POWER_SUPPLY_PROP_CURRENT_MAX] = 5,
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

	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = priv->batt_info.energy_full_design_uwh;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = priv->batt_info.charge_full_design_uah;
		break;

	case POWER_SUPPLY_PROP_TEMP_MIN:
		if (priv->batt_info.temp_min == INT_MIN)
			return -ENODATA;

		val->intval = priv->batt_info.temp_min * 10;
		break;

	case POWER_SUPPLY_PROP_TEMP_MAX:
		if (priv->batt_info.temp_max == INT_MAX)
			return -ENODATA;

		val->intval = priv->batt_info.temp_max * 10;
		break;

	default:
		ret = pad_battery_get_value(priv, psp);
		if (ret < 0)
			return ret;

		val->intval = (s16)ret;

		switch (psp) {
		case POWER_SUPPLY_PROP_STATUS:
			if (ret & ASUSEC_BATTERY_FULL_CHARGED)
				val->intval = POWER_SUPPLY_STATUS_FULL;
			else if (ret & ASUSEC_BATTERY_FULL_DISCHARGED)
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			else if (ret & ASUSEC_BATTERY_DISCHARGING)
				val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			else
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			break;

		case POWER_SUPPLY_PROP_TEMP:
			val->intval -= TEMP_CELSIUS_OFFSET;
			break;

		case POWER_SUPPLY_PROP_CHARGE_NOW:
		case POWER_SUPPLY_PROP_CURRENT_NOW:
		case POWER_SUPPLY_PROP_CURRENT_MAX:
		case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		case POWER_SUPPLY_PROP_VOLTAGE_MAX:
			val->intval *= 1000;
			break;

		default:
			break;
		}

		break;
	}

	return 0;
}

static int asusec_battery_no_usb(struct asusec_battery_data *priv)
{
	int ret = 0;
	u8 buf[DOCKRAM_ENTRY_BUFSIZE];
	char *plug = buf;

	ret = asus_dockram_read(priv->ec->dockram, priv->charger_addr, plug);
	if (ret < 0)
		return -EINVAL;

	ret = plug[1] & ASUSEC_CHARGER_USB_MASK;
	if (ret == ASUSEC_CHARGER_USB_MASK)
		return 0;

	return ret;
}

static void asusec_battery_poll_work(struct work_struct *work)
{
	struct asusec_battery_data *priv =
		container_of(work, struct asusec_battery_data, poll_work.work);
	int state;

	state = pad_battery_get_value(priv, POWER_SUPPLY_PROP_STATUS);
	if (state < 0)
		return;

	if (state & ASUSEC_BATTERY_FULL_CHARGED)
		state = POWER_SUPPLY_STATUS_FULL;
	else if (state & ASUSEC_BATTERY_DISCHARGING)
		state = POWER_SUPPLY_STATUS_DISCHARGING;
	else
		state = POWER_SUPPLY_STATUS_CHARGING;

	if (priv->last_state != state && asusec_battery_no_usb(priv)) {
		priv->last_state = state;
		power_supply_changed(priv->battery);
	}

	/* continuously send uevent notification */
	schedule_delayed_work(&priv->poll_work,
			      msecs_to_jiffies(ASUSEC_BATTERY_DATA_FRESH_MSEC));
}

static const struct power_supply_desc pad_battery_desc = {
	.name = "pad-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = pad_battery_properties,
	.num_properties = ARRAY_SIZE(pad_battery_properties),
	.get_property = pad_battery_get_property,
	.external_power_changed = power_supply_changed,
};

static const struct power_supply_desc dock_battery_desc = {
	.name = "dock-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = pad_battery_properties,
	.num_properties = ARRAY_SIZE(pad_battery_properties),
	.get_property = pad_battery_get_property,
	.external_power_changed = power_supply_changed,
};

static int asusec_battery_probe(struct platform_device *pdev)
{
	const struct asusec_info *ec = asusec_cell_to_ec(pdev);
	const struct power_supply_desc *psd;
	struct asusec_battery_data *priv;
	struct asusec_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct power_supply_config cfg = {};

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	mutex_init(&priv->battery_lock);

	priv->ec = ec;
	priv->battery_addr = pdata->battery_addr;
	priv->charger_addr = pdata->charger_addr;
	priv->batt_data_ts = jiffies - 1;
	priv->last_state = POWER_SUPPLY_STATUS_UNKNOWN;

	cfg.of_node = pdev->dev.parent->of_node;
	cfg.drv_data = priv;

	if (of_device_is_compatible(cfg.of_node, "asus,pad-ec"))
		psd = &pad_battery_desc;
	else
		psd = &dock_battery_desc;

	priv->battery = devm_power_supply_register(&pdev->dev, psd, &cfg);
	if (IS_ERR(priv->battery))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->battery),
				     "Failed to register power supply\n");

	if (power_supply_get_battery_info(priv->battery, &priv->batt_info))
		dev_warn(&pdev->dev,
			 "No monitored battery, some properties will be missing\n");

	INIT_DELAYED_WORK(&priv->poll_work, asusec_battery_poll_work);
	schedule_delayed_work(&priv->poll_work,
			      msecs_to_jiffies(ASUSEC_BATTERY_DATA_FRESH_MSEC));

	return 0;
}

static int asusec_battery_remove(struct platform_device *pdev)
{
	struct asusec_battery_data *priv = dev_get_drvdata(&pdev->dev);

	cancel_delayed_work_sync(&priv->poll_work);

	return 0;
}

static int __maybe_unused asusec_battery_suspend(struct device *dev)
{
	struct asusec_battery_data *priv = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&priv->poll_work);

	return 0;
}

static int __maybe_unused asusec_battery_resume(struct device *dev)
{
	struct asusec_battery_data *priv = dev_get_drvdata(dev);

	schedule_delayed_work(&priv->poll_work,
			      msecs_to_jiffies(ASUSEC_BATTERY_DATA_FRESH_MSEC));

	return 0;
}

static SIMPLE_DEV_PM_OPS(asusec_battery_pm_ops,
			 asusec_battery_suspend, asusec_battery_resume);

static struct platform_driver asusec_battery_driver = {
	.driver = {
		.name = "asusec-battery",
		.pm = &asusec_battery_pm_ops,
	},
	.probe = asusec_battery_probe,
	.remove = asusec_battery_remove,
};
module_platform_driver(asusec_battery_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer Pad battery driver");
MODULE_LICENSE("GPL");
