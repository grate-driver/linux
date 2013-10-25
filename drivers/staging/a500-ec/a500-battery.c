// SPDX-License-Identifier: GPL-2.0+
/*
 * Battery driver for Acer Iconia Tab A500
 *
 * Based on original Acer battery driver.
 * Based on NVIDIA Gas Gauge driver for SBS Compliant Batteries.
 *
 * Copyright (c) 2010, NVIDIA Corporation.
 */

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "ec.h"

enum {
	REG_CAPACITY,
	REG_VOLTAGE,
	REG_CURRENT,
	REG_DESIGN_CAPACITY,
	REG_HEALTH,
	REG_TEMPERATURE,
	REG_SERIAL_NUMBER,
};

#define EC_DATA(_addr, _timeout, _psp) {	\
	.psp = POWER_SUPPLY_PROP_ ## _psp,	\
	.reg_data = {				\
		.addr = _addr,			\
		.timeout = _timeout		\
	},					\
}

static struct chip_data {
	enum power_supply_property psp;
	struct ec_reg_data reg_data;
} ec_data[] = {
	[REG_CAPACITY]		= EC_DATA(0x00,  0, CAPACITY),
	[REG_VOLTAGE]		= EC_DATA(0x01,  0, VOLTAGE_NOW),
	[REG_CURRENT]		= EC_DATA(0x03, 10, CURRENT_NOW),
	[REG_DESIGN_CAPACITY]	= EC_DATA(0x08,  0, CHARGE_FULL_DESIGN),
	[REG_HEALTH]		= EC_DATA(0x09, 10, HEALTH),
	[REG_TEMPERATURE]	= EC_DATA(0x0a,  0, TEMP),
	[REG_SERIAL_NUMBER]	= EC_DATA(0x6a,  0, SERIAL_NUMBER),
};

static enum power_supply_property ec_properties[] = {
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

struct ec_battery_info {
	struct delayed_work		poll_work;
	struct power_supply		*bat;
	struct power_supply_desc	bat_desc;
	unsigned int			capacity;
};

static int ec_get_battery_presence(union power_supply_propval *val)
{
	s32 ret;

	ret = a500_ec_read_word_data(&ec_data[REG_DESIGN_CAPACITY].reg_data);
	if (ret <= 0)
		val->intval = 0;
	else
		val->intval = 1;

	return 0;
}

static int ec_get_battery_health(union power_supply_propval *val)
{
	s32 ret;

	ret = a500_ec_read_word_data(&ec_data[REG_HEALTH].reg_data);
	if (ret < 0)
		return ret;

	if (ret > 50)
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
	else
		val->intval = POWER_SUPPLY_HEALTH_DEAD;

	return 0;
}

static bool ec_get_battery_capacity(struct ec_battery_info *chip)
{
	unsigned int capacity;
	s32 ret;

	ret = a500_ec_read_word_data(&ec_data[REG_CAPACITY].reg_data);
	if (ret < 0)
		return false;

	/* capacity can be >100% even if max value is 100% */
	capacity = min(ret, 100);

	if (chip->capacity != capacity) {
		chip->capacity = capacity;
		return true;
	}

	return false;
}

static void ec_get_battery_status(struct ec_battery_info *chip,
				  union power_supply_propval *val)
{
	if (chip->capacity < 100) {
		if (power_supply_am_i_supplied(chip->bat))
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
	} else {
		val->intval = POWER_SUPPLY_STATUS_FULL;
	}
}

static int ec_get_battery_property(int reg_offset,
				   union power_supply_propval *val)
{
	s32 ret;

	ret = a500_ec_read_word_data(&ec_data[reg_offset].reg_data);
	if (ret < 0)
		return ret;

	val->intval = ret;

	return 0;
}

static void ec_unit_adjustment(struct device *dev,
			       enum power_supply_property psp,
			       union power_supply_propval *val)
{
#define BASE_UNIT_CONVERSION		1000
#define TEMP_KELVIN_TO_CELSIUS		2731
	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = (s16)val->intval;
	/* fall through */
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval *= BASE_UNIT_CONVERSION;
		break;

	case POWER_SUPPLY_PROP_TEMP:
		val->intval -= TEMP_KELVIN_TO_CELSIUS;
		break;

	default:
		dev_dbg(dev,
			"%s: no need for unit conversion %d\n", __func__, psp);
	}
}

#define SERIAL_PARTS_NB	11
#define SERIAL_STRLEN	(SERIAL_PARTS_NB * 2 + 1)
static char ec_serial[SERIAL_STRLEN];

static int ec_get_battery_serial_number(union power_supply_propval *val)
{
	unsigned int i;
	s32 ret = 0;

	if (!ec_serial[0]) {
		a500_ec_lock();
		for (i = 0; i < SERIAL_PARTS_NB; i++) {
			ret = a500_ec_read_word_data_locked(
					&ec_data[REG_SERIAL_NUMBER].reg_data);
			if (ret < 0) {
				ec_serial[0] = '\0';
				break;
			}

			ec_serial[i * 2 + 0] = (ret >> 0) & 0xff;
			ec_serial[i * 2 + 1] = (ret >> 8) & 0xff;
		}
		a500_ec_unlock();
	}

	if (ret < 0)
		return ret;

	val->strval = ec_serial;

	return 0;
}

static int ec_get_property_index(struct device *dev,
				 enum power_supply_property psp)
{
	unsigned int count;

	for (count = 0; count < ARRAY_SIZE(ec_data); count++)
		if (psp == ec_data[count].psp)
			return count;

	dev_warn(dev, "%s: invalid property - %d\n", __func__, psp);

	return -EINVAL;
}

static int ec_get_property(struct power_supply *psy,
			   enum power_supply_property psp,
			   union power_supply_propval *val)
{
	struct ec_battery_info *chip = power_supply_get_drvdata(psy);
	struct device *dev = psy->dev.parent;
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		ret = ec_get_battery_serial_number(val);
		break;

	case POWER_SUPPLY_PROP_HEALTH:
		ret = ec_get_battery_health(val);
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		ret = ec_get_battery_presence(val);
		break;

	case POWER_SUPPLY_PROP_STATUS:
		ec_get_battery_status(chip, val);
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		ec_get_battery_capacity(chip);
		val->intval = chip->capacity;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_TEMP:
		ret = ec_get_property_index(dev, psp);
		if (ret < 0)
			break;

		ret = ec_get_battery_property(ret, val);
		break;

	default:
		dev_err(dev, "%s: invalid property\n", __func__);
		return -EINVAL;
	}

	if (!ret) {
		/* Convert units to match requirements for power supply class */
		ec_unit_adjustment(dev, psp, val);
	}

	dev_dbg(dev, "%s: property = %d, value = %x\n",
		__func__, psp, val->intval);

	/* battery not present, so return NODATA for properties */
	if (ret)
		return -ENODATA;

	return 0;
}

static void ec_delayed_work(struct work_struct *work)
{
	struct ec_battery_info *chip;
	bool capacity_changed;

	chip = container_of(work, struct ec_battery_info, poll_work.work);
	capacity_changed = ec_get_battery_capacity(chip);

	if (capacity_changed)
		power_supply_changed(chip->bat);

	/* send continuous uevent notify */
	schedule_delayed_work(&chip->poll_work, 60 * HZ);
}

static int ec_probe(struct platform_device *pdev)
{
	struct power_supply_config psy_cfg = {};
	struct ec_battery_info *chip;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	psy_cfg.of_node = pdev->dev.of_node;
	psy_cfg.drv_data = chip;

	chip->bat_desc.name = "embedded-controller";
	chip->bat_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	chip->bat_desc.properties = ec_properties;
	chip->bat_desc.num_properties = ARRAY_SIZE(ec_properties);
	chip->bat_desc.get_property = ec_get_property;
	chip->bat_desc.external_power_changed = power_supply_changed;

	chip->bat = power_supply_register_no_ws(&pdev->dev, &chip->bat_desc,
						&psy_cfg);
	if (IS_ERR(chip->bat)) {
		dev_err(&pdev->dev, "failed to register power supply: %pe\n",
			chip->bat);
		return PTR_ERR(chip->bat);
	}

	INIT_DELAYED_WORK(&chip->poll_work, ec_delayed_work);
	schedule_work(&chip->poll_work.work);

	platform_set_drvdata(pdev, chip);

	return 0;
}

static int ec_remove(struct platform_device *pdev)
{
	struct ec_battery_info *chip = dev_get_drvdata(&pdev->dev);

	cancel_delayed_work_sync(&chip->poll_work);
	power_supply_unregister(chip->bat);

	return 0;
}

static int __maybe_unused ec_suspend(struct device *dev)
{
	struct ec_battery_info *chip = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&chip->poll_work);

	return 0;
}

static int __maybe_unused ec_resume(struct device *dev)
{
	struct ec_battery_info *chip = dev_get_drvdata(dev);

	schedule_delayed_work(&chip->poll_work, HZ);

	return 0;
}

static SIMPLE_DEV_PM_OPS(ec_battery_pm_ops, ec_suspend, ec_resume);

static const struct of_device_id ec_battery_match[] = {
	{ .compatible = "acer,a500-iconia-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, ec_battery_match);

static struct platform_driver ec_battery_driver = {
	.driver = {
		.name = "a500-battery",
		.pm = &ec_battery_pm_ops,
		.of_match_table = ec_battery_match,
	},
	.probe = ec_probe,
	.remove = ec_remove,
};
module_platform_driver(ec_battery_driver);

MODULE_DESCRIPTION("Acer Iconia Tab A500 Embedded Controller battery driver");
MODULE_AUTHOR("Dmitry Osipenko <digetx@gmail.com>");
MODULE_LICENSE("GPL v2");
