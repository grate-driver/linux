// SPDX-License-Identifier: GPL-2.0+
// Battery and Charger driver for Surface RT
// Based on the ACPI Control Method Interface

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/types.h>

// Register Addresses (B=byte; W=word; S=string)
#define REGB_STATUS 0x02
#define REGW_VOLTAGE_NOW 0x20
#define REGW_CURRENT_NOW 0x24
#define REGW_CAPACITY 0x28
#define REGW_CHARGE_FULL 0x2C
#define REGW_CYCLE_COUNT 0x3A
#define REGW_CHARGE_FULL_DESIGN 0x3C
#define REGW_VOLTAGE_MAX_DESIGN 0x3E
#define REGW_SERIAL_NUMBER 0x44
#define REGS_MANUFACTURER 0x46
#define REGS_MODEL_NAME 0x52
#define REGS_TECHNOLOGY 0x5A
#define REGB_ONLINE 0x67
#define REG_CHARGE_NOW 0xff

struct srt_ec_device {
	struct i2c_client *client;
	struct device *dev;
	struct power_supply *bat;
	struct power_supply *psy;
	struct gpio_desc *enable_gpio;
	struct delayed_work poll_work;
	u8 capacity;
};

static enum power_supply_property srt_bat_power_supply_props[] = {
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static enum power_supply_property srt_psy_power_supply_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int srt_bat_get_string(struct i2c_client *client, char *buf, u8 reg)
{
	struct i2c_msg msg[2];

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = 1;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = buf;

	switch (reg) {
	case REGS_MANUFACTURER:
		msg[1].len = 12;
		i2c_transfer(client->adapter, msg, 2);
		break;
	case REGS_MODEL_NAME:
		msg[1].len = 9;
		i2c_transfer(client->adapter, msg, 2);
		break;
	case REGW_SERIAL_NUMBER:
		sprintf(buf, "%04x", i2c_smbus_read_word_data(client, reg));
		break;
	case REGS_TECHNOLOGY:
		msg[1].len = 4;
		i2c_transfer(client->adapter, msg, 2);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int srt_bat_get_value(struct i2c_client *client, int reg)
{
	static char str_buf[4];

	switch (reg) {
	case REGW_CHARGE_FULL_DESIGN:
	case REGW_CHARGE_FULL:
	case REGW_VOLTAGE_MAX_DESIGN:
	case REGW_VOLTAGE_NOW:
		return i2c_smbus_read_word_data(client, reg) * 1000;
	case REGW_CURRENT_NOW:
		return (int16_t)i2c_smbus_read_word_data(client, reg) * 1000;
	case REGW_CAPACITY:
	case REGW_CYCLE_COUNT:
		return i2c_smbus_read_word_data(client, reg);
	case REGB_STATUS:
		if (i2c_smbus_read_byte_data(client, reg) & 0x01)
			return POWER_SUPPLY_STATUS_CHARGING;
		else
			return POWER_SUPPLY_STATUS_DISCHARGING;
	case REGB_ONLINE:
		return (i2c_smbus_read_byte_data(client, reg) & 0x02) >> 1;
	case REG_CHARGE_NOW:
		return i2c_smbus_read_word_data(client, REGW_CAPACITY)
		       * i2c_smbus_read_word_data(client, REGW_CHARGE_FULL) * 10;
		break;
	case REGS_TECHNOLOGY:
		srt_bat_get_string(client, str_buf, reg);
		if (strncmp(str_buf, "LION", 4) == 0)
			return POWER_SUPPLY_TECHNOLOGY_LION;
		else
			return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
	default:
		return -EINVAL;
	}
}

static int srt_bat_power_supply_get_property(struct power_supply *psy,
					     enum power_supply_property psp,
					     union power_supply_propval *val)
{
	struct srt_ec_device *srt_ec = power_supply_get_drvdata(psy);
	struct i2c_client *client = srt_ec->client;
	static char str_buf[12];

	switch (psp) {
	case POWER_SUPPLY_PROP_MANUFACTURER:
		srt_bat_get_string(client, str_buf, REGS_MANUFACTURER);
		val->strval = str_buf;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		srt_bat_get_string(client, str_buf, REGS_MODEL_NAME);
		val->strval = str_buf;
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		srt_bat_get_string(client, str_buf, REGW_SERIAL_NUMBER);
		val->strval = str_buf;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = srt_bat_get_value(client, REGW_CAPACITY);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = srt_bat_get_value(client, REGW_CHARGE_FULL);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = srt_bat_get_value(client, REGW_CHARGE_FULL_DESIGN);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = srt_bat_get_value(client, REG_CHARGE_NOW);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = srt_bat_get_value(client, REGW_CURRENT_NOW);
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = srt_bat_get_value(client, REGW_CYCLE_COUNT);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = srt_bat_get_value(client, REGB_ONLINE);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = srt_bat_get_value(client, REGB_STATUS);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = srt_bat_get_value(client, REGS_TECHNOLOGY);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = srt_bat_get_value(client, REGW_VOLTAGE_MAX_DESIGN);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = srt_bat_get_value(client, REGW_VOLTAGE_NOW);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int srt_psy_power_supply_get_property(struct power_supply *psy,
					     enum power_supply_property psp,
					     union power_supply_propval *val)
{
	struct srt_ec_device *srt_ec = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = i2c_smbus_read_byte_data(srt_ec->client, REGB_ONLINE) & 0x01;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void srt_bat_poll_work(struct work_struct *work)
{
	struct srt_ec_device *ec;
	u8 capacity;

	ec = container_of(work, struct srt_ec_device, poll_work.work);

	capacity = srt_bat_get_value(ec->client, REGW_CAPACITY);

	if (capacity != ec->capacity) {
		ec->capacity = capacity;
		power_supply_changed(ec->bat);
	}

	/* continuously send uevent notification */
	schedule_delayed_work(&ec->poll_work, 30 * HZ);
}

static irqreturn_t srt_psy_detect_irq(int irq, void *dev_id)
{
	struct srt_ec_device *ec = dev_id;

	power_supply_changed(ec->psy);
	return IRQ_HANDLED;
}

static const struct power_supply_desc srt_bat_power_supply_desc = {
	.name = "surface-rt-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = srt_bat_power_supply_props,
	.num_properties = ARRAY_SIZE(srt_bat_power_supply_props),
	.get_property = srt_bat_power_supply_get_property,
	.external_power_changed = power_supply_changed,
};

static const struct power_supply_desc srt_psy_power_supply_desc = {
	.name = "surface-rt-ac-adapter",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = srt_psy_power_supply_props,
	.num_properties = ARRAY_SIZE(srt_psy_power_supply_props),
	.get_property = srt_psy_power_supply_get_property,
	.external_power_changed = power_supply_changed,
};

static int srt_ec_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct srt_ec_device *srt_ec;
	struct power_supply_config bat_cfg = {};
	struct power_supply_config psy_cfg = {};
	int ret = -1;

	srt_ec = devm_kzalloc(dev, sizeof(*srt_ec), GFP_KERNEL);
	if (!srt_ec)
		return -ENOMEM;

	srt_ec->client = client;
	srt_ec->dev = dev;

	srt_ec->enable_gpio = devm_gpiod_get(srt_ec->dev,
					     "enable",
					     GPIOD_OUT_HIGH);
	if (IS_ERR(srt_ec->enable_gpio)) {
		ret = PTR_ERR(srt_ec->enable_gpio);
		dev_err(srt_ec->dev, "Failed to get enable gpio\n");
		return ret;
	}
	usleep_range(1000, 1500); // 1ms from ACPI wait till EC is ready

	bat_cfg.drv_data = srt_ec;
	srt_ec->bat = devm_power_supply_register(srt_ec->dev,
						 &srt_bat_power_supply_desc,
						 &bat_cfg);
	if (PTR_ERR_OR_ZERO(srt_ec->bat) < 0) {
		dev_err(srt_ec->dev, "Failed to register battery power supply\n");
		return ret;
	}

	psy_cfg.drv_data = srt_ec;
	srt_ec->psy = devm_power_supply_register(srt_ec->dev,
						 &srt_psy_power_supply_desc,
						 &psy_cfg);
	if (PTR_ERR_OR_ZERO(srt_ec->psy) < 0) {
		dev_err(srt_ec->dev, "Failed to register AC power supply\n");
		return ret;
	}

	i2c_set_clientdata(client, srt_ec);

	ret = request_threaded_irq(client->irq, NULL, srt_psy_detect_irq,
				   IRQF_ONESHOT, client->name, srt_ec);
	if (ret < 0) {
		dev_warn(&client->dev,
			 "Could not register for %s interrupt, irq = %d, err = %d\n",
			 client->name, client->irq, ret);
		return ret;
	}

	INIT_DELAYED_WORK(&srt_ec->poll_work, srt_bat_poll_work);
	schedule_delayed_work(&srt_ec->poll_work, HZ);

	return 0;
}

static int srt_ec_remove(struct i2c_client *client)
{
	struct srt_ec_device *ec = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&ec->poll_work);
	power_supply_unregister(ec->bat);
	power_supply_unregister(ec->psy);

	return 0;
}

static const struct i2c_device_id srt_ec_i2c_ids[] = {
	{ "surface-rt-ec", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, srt_ec_i2c_ids);

static const struct of_device_id srt_ec_of_match[] = {
	{ .compatible = "microsoft,surface-rt-ec", },
	{},
};
MODULE_DEVICE_TABLE(of, srt_ec_of_match);

static struct i2c_driver srt_ec_driver = {
	.driver = {
		.name = "surface-rt-ec",
		.of_match_table = of_match_ptr(srt_ec_of_match),
	},
	.probe = srt_ec_probe,
	.remove = srt_ec_remove,
	.id_table = srt_ec_i2c_ids,
};
module_i2c_driver(srt_ec_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonas Schwöbel <jonasschwoebel@yahoo.de>");
MODULE_DESCRIPTION("Surface RT EmbeddedController driver");
