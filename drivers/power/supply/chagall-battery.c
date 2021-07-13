// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Battery Gas Gauge driver for Pegatron Chagall
 *
 * Written by: Svyatoslav Ryhel <clamor95@gmail.com>
 *
 * Copyright (C) 2021 Svyatoslav Ryhel
 *
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/power_supply.h>

#define CHAGALL_BATTERY_RETRY_MAX			5
#define CHAGALL_BATTERY_DATA_REFRESH			5000

#define CHAGALL_BATTERY_DISCHARGING			0x40
#define CHAGALL_BATTERY_FULL_CHARGED			0x20
#define CHAGALL_BATTERY_FULL_DISCHARGED			0x10

#define CHAGALL_BATTERY_INFO_BLOCK_START_ADDR		0xa0
#define CHAGALL_BATTERY_INFO_BLOCK_SIZE			20

#define CHAGALL_REG_AMBER_LED				0x60
#define CHAGALL_REG_WHITE_LED				0x70
#define CHAGALL_REG_LED_ON				0x01
#define CHAGALL_REG_LED_OFF				0x00

#define TEMP_CELSIUS_OFFSET				2731

struct chagall_battery_data {
	struct i2c_client				*client;
	struct power_supply				*battery;
	struct power_supply_battery_info		batt_info;
	struct delayed_work				poll_work;
	struct mutex					battery_lock;
	unsigned long					batt_data_ts;
	int						last_state;
	u8						batt_data[CHAGALL_BATTERY_INFO_BLOCK_SIZE];
	bool						amber_on;
	bool						white_on;
};

static int chagall_battery_refresh(struct chagall_battery_data *cg)
{
	int i, ret = 0;

	mutex_lock(&cg->battery_lock);

	if (time_before(jiffies, cg->batt_data_ts))
		goto out_unlock;

	for (i = 0; i < CHAGALL_BATTERY_RETRY_MAX; i++) {
		ret = i2c_smbus_read_i2c_block_data(cg->client,
			CHAGALL_BATTERY_INFO_BLOCK_START_ADDR,
			CHAGALL_BATTERY_INFO_BLOCK_SIZE, cg->batt_data);
		if (ret >= 0)
			break;
	}

	cg->batt_data_ts = jiffies + msecs_to_jiffies(CHAGALL_BATTERY_DATA_REFRESH);

out_unlock:
	mutex_unlock(&cg->battery_lock);

	return ret;
}

static int chagall_write_reg(struct chagall_battery_data *cg,
			     u8 reg_addr, u8 value, bool state)
{
	unsigned char buf[] = { reg_addr, value };
	int i, ret = 0;

	for (i = 0; i < CHAGALL_BATTERY_RETRY_MAX; i++) {
		ret = i2c_master_send(cg->client, buf, sizeof(buf));
		if (ret >= 0)
			break;
	}

	if (ret < 0)
		return -EINVAL;

	state = !!value;

	return 0;
}

static enum power_supply_property chagall_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_TEMP_MIN,
	POWER_SUPPLY_PROP_TEMP_MAX,
};

static const unsigned int chagall_battery_prop_offs[] = {
	[POWER_SUPPLY_PROP_TEMP] = 2,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = 4,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = 6,
	[POWER_SUPPLY_PROP_CAPACITY] = 8,
	[POWER_SUPPLY_PROP_CURRENT_MAX] = 10,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = 12,
	[POWER_SUPPLY_PROP_STATUS] = 14,
	[POWER_SUPPLY_PROP_CHARGE_NOW] = 16,
	[POWER_SUPPLY_PROP_CHARGE_FULL] = 18,
};

static void chagall_leds_status_update(struct chagall_battery_data *cg,
				       int state)
{
	switch (state) {
	case POWER_SUPPLY_STATUS_FULL:
		if (!cg->white_on)
			chagall_write_reg(cg, CHAGALL_REG_WHITE_LED,
					  CHAGALL_REG_LED_ON, cg->white_on);
		if (cg->amber_on)
			chagall_write_reg(cg, CHAGALL_REG_AMBER_LED,
					  CHAGALL_REG_LED_OFF, cg->amber_on);
		break;

	case POWER_SUPPLY_STATUS_CHARGING:
		if (!cg->amber_on)
			chagall_write_reg(cg, CHAGALL_REG_AMBER_LED,
					  CHAGALL_REG_LED_ON, cg->amber_on);
		if (cg->white_on)
			chagall_write_reg(cg, CHAGALL_REG_WHITE_LED,
					  CHAGALL_REG_LED_OFF, cg->white_on);
		break;

	default:
		if (cg->amber_on)
			chagall_write_reg(cg, CHAGALL_REG_AMBER_LED,
					  CHAGALL_REG_LED_OFF, cg->amber_on);
		if (cg->white_on)
			chagall_write_reg(cg, CHAGALL_REG_WHITE_LED,
					  CHAGALL_REG_LED_OFF, cg->white_on);
		break;
	}
}

static int chagall_battery_get_value(struct chagall_battery_data *cg,
				     enum power_supply_property psp)
{
	int ret, offs;

	if (psp >= ARRAY_SIZE(chagall_battery_prop_offs))
		return -EINVAL;
	if (!chagall_battery_prop_offs[psp])
		return -EINVAL;

	ret = chagall_battery_refresh(cg);
	if (ret < 0)
		return ret;

	offs = chagall_battery_prop_offs[psp];
	return (cg->batt_data[offs + 1] << 8) + cg->batt_data[offs];
}

static int chagall_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct chagall_battery_data *cg = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;

	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = cg->batt_info.energy_full_design_uwh;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = cg->batt_info.charge_full_design_uah;
		break;

	case POWER_SUPPLY_PROP_TEMP_MIN:
		if (cg->batt_info.temp_min == INT_MIN)
			return -ENODATA;

		val->intval = cg->batt_info.temp_min * 10;
		break;

	case POWER_SUPPLY_PROP_TEMP_MAX:
		if (cg->batt_info.temp_max == INT_MAX)
			return -ENODATA;

		val->intval = cg->batt_info.temp_max * 10;
		break;

	default:
		ret = chagall_battery_get_value(cg, psp);
		if (ret < 0)
			return ret;

		val->intval = (s16)ret;

		switch (psp) {
		case POWER_SUPPLY_PROP_TEMP:
			val->intval -= TEMP_CELSIUS_OFFSET;
			break;

		case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		case POWER_SUPPLY_PROP_CURRENT_MAX:
		case POWER_SUPPLY_PROP_CURRENT_NOW:
		case POWER_SUPPLY_PROP_CHARGE_FULL:
		case POWER_SUPPLY_PROP_CHARGE_NOW:
			val->intval *= 1000;
			break;

		case POWER_SUPPLY_PROP_STATUS:
			if (ret & CHAGALL_BATTERY_FULL_CHARGED)
				val->intval = POWER_SUPPLY_STATUS_FULL;
			else if (ret & CHAGALL_BATTERY_FULL_DISCHARGED)
				val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			else if (ret & CHAGALL_BATTERY_DISCHARGING)
				val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			else
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			break;

		default:
			break;
		}

		break;
	}

	return 0;
}

static void chagall_battery_poll_work(struct work_struct *work)
{
	struct chagall_battery_data *cg =
		container_of(work, struct chagall_battery_data, poll_work.work);
	int state;

	state = chagall_battery_get_value(cg, POWER_SUPPLY_PROP_STATUS);
	if (state < 0)
		return;

	if (state & CHAGALL_BATTERY_FULL_CHARGED)
		state = POWER_SUPPLY_STATUS_FULL;
	else if (state & CHAGALL_BATTERY_DISCHARGING)
		state = POWER_SUPPLY_STATUS_DISCHARGING;
	else
		state = POWER_SUPPLY_STATUS_CHARGING;

	if (cg->last_state != state) {
		cg->last_state = state;
		power_supply_changed(cg->battery);
	}

	chagall_leds_status_update(cg, state);

	/* continuously send uevent notification */
	schedule_delayed_work(&cg->poll_work,
			      msecs_to_jiffies(CHAGALL_BATTERY_DATA_REFRESH));
}

static const struct power_supply_desc chagall_battery_desc = {
	.name = "chagall-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = chagall_battery_properties,
	.num_properties = ARRAY_SIZE(chagall_battery_properties),
	.get_property = chagall_battery_get_property,
};

static int chagall_battery_probe(struct i2c_client *client)
{
	struct chagall_battery_data *cg;
	struct power_supply_config cfg = { };

	cg = devm_kzalloc(&client->dev, sizeof(*cg), GFP_KERNEL);
	if (!cg)
		return -ENOMEM;

	mutex_init(&cg->battery_lock);

	cg->client = client;
	cg->batt_data_ts = jiffies - 1;
	cg->last_state = POWER_SUPPLY_STATUS_UNKNOWN;

	cfg.of_node = client->dev.of_node;
	cfg.drv_data = cg;

	i2c_set_clientdata(client, &cg->client);

	cg->battery = devm_power_supply_register(&client->dev, &chagall_battery_desc, &cfg);
	if (IS_ERR(cg->battery))
		return PTR_ERR(cg->battery);

	if (power_supply_get_battery_info(cg->battery, &cg->batt_info))
		dev_warn(&client->dev,
			 "No monitored battery, some properties will be missing\n");

	chagall_write_reg(cg, CHAGALL_REG_AMBER_LED, CHAGALL_REG_LED_OFF,
			  cg->amber_on);
	chagall_write_reg(cg, CHAGALL_REG_WHITE_LED, CHAGALL_REG_LED_OFF,
			  cg->white_on);

	INIT_DELAYED_WORK(&cg->poll_work, chagall_battery_poll_work);
	schedule_delayed_work(&cg->poll_work,
			      msecs_to_jiffies(CHAGALL_BATTERY_DATA_REFRESH));

	return 0;
}

static int chagall_battery_remove(struct i2c_client *client)
{
	struct chagall_battery_data *cg = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&cg->poll_work);

	return 0;
}

static int __maybe_unused chagall_battery_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct chagall_battery_data *cg = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&cg->poll_work);

	return 0;
}

static int __maybe_unused chagall_battery_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct chagall_battery_data *cg = i2c_get_clientdata(client);

	schedule_delayed_work(&cg->poll_work,
			      msecs_to_jiffies(CHAGALL_BATTERY_DATA_REFRESH));

	return 0;
}

static SIMPLE_DEV_PM_OPS(chagall_battery_pm_ops,
			 chagall_battery_suspend, chagall_battery_resume);

static const struct of_device_id chagall_match[] = {
	{ .compatible = "pegatron,chagall-ec" },
	{ }
};
MODULE_DEVICE_TABLE(of, chagall_match);

static struct i2c_driver chagall_battery_driver = {
	.driver = {
		.name = "chagall-battery",
		.pm = &chagall_battery_pm_ops,
		.of_match_table = chagall_match,
	},
	.probe_new = chagall_battery_probe,
	.remove = chagall_battery_remove,
};
module_i2c_driver(chagall_battery_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("Pegatron Chagall fuel gauge driver");
MODULE_LICENSE("GPL");
