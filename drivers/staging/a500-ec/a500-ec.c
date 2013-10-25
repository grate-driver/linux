// SPDX-License-Identifier: GPL-2.0+
/*
 * MFD driver for Acer Iconia Tab A500 Embedded Controller
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/irqflags.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/reboot.h>

#include <linux/mfd/core.h>

#include "ec.h"

/*				addr	timeout ms */
EC_REG_DATA(SHUTDOWN,		0x52,	0);
EC_REG_DATA(WARM_REBOOT,	0x54,	0);
EC_REG_DATA(COLD_REBOOT,	0x55,	1000);

static DEFINE_MUTEX(ec_mutex);

static struct ec_info {
	struct i2c_client *client;
} *ec_chip;

void a500_ec_lock(void)
{
	if (WARN_ON_ONCE(!ec_chip))
		return;

	mutex_lock(&ec_mutex);
}
EXPORT_SYMBOL_GPL(a500_ec_lock);

void a500_ec_unlock(void)
{
	if (!ec_chip)
		return;

	mutex_unlock(&ec_mutex);
}
EXPORT_SYMBOL_GPL(a500_ec_unlock);

#define I2C_ERR_TIMEOUT	500
int a500_ec_read_word_data_locked(const struct ec_reg_data *reg_data)
{
	struct i2c_client *client;
	unsigned int retries = 5;
	s32 ret = 0;

	if (WARN_ON_ONCE(!ec_chip))
		return -EINVAL;

	client = ec_chip->client;

	while (retries-- > 0) {
		ret = i2c_smbus_read_word_data(client, reg_data->addr);
		if (ret >= 0)
			break;
		msleep(I2C_ERR_TIMEOUT);
	}

	if (ret < 0) {
		dev_err(&client->dev, "i2c read at address 0x%x failed: %d\n",
			reg_data->addr, ret);
		return ret;
	}

	msleep(reg_data->timeout);

	return le16_to_cpu(ret);
}
EXPORT_SYMBOL_GPL(a500_ec_read_word_data_locked);

int a500_ec_read_word_data(const struct ec_reg_data *reg_data)
{
	s32 ret;

	if (WARN_ON_ONCE(!ec_chip))
		return -EINVAL;

	a500_ec_lock();
	ret = a500_ec_read_word_data_locked(reg_data);
	a500_ec_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(a500_ec_read_word_data);

int a500_ec_write_word_data_locked(const struct ec_reg_data *reg_data,
				   u16 value)
{
	struct i2c_client *client;
	unsigned int retries = 5;
	s32 ret = 0;

	if (WARN_ON_ONCE(!ec_chip))
		return -EINVAL;

	client = ec_chip->client;

	while (retries-- > 0) {
		ret = i2c_smbus_write_word_data(client, reg_data->addr,
						le16_to_cpu(value));
		if (ret >= 0)
			break;

		if (!irqs_disabled())
			msleep(I2C_ERR_TIMEOUT);
	}

	if (ret < 0) {
		dev_err(&client->dev, "i2c write to address 0x%x failed: %d\n",
			reg_data->addr, ret);
		return ret;
	}

	if (!irqs_disabled())
		msleep(reg_data->timeout);

	return 0;
}
EXPORT_SYMBOL_GPL(a500_ec_write_word_data_locked);

int a500_ec_write_word_data(const struct ec_reg_data *reg_data, u16 value)
{
	s32 ret;

	if (WARN_ON_ONCE(!ec_chip))
		return -EINVAL;

	a500_ec_lock();
	ret = a500_ec_write_word_data_locked(reg_data, value);
	a500_ec_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(a500_ec_write_word_data);

static void ec_poweroff(void)
{
	dev_info(&ec_chip->client->dev, "poweroff ...\n");

	a500_ec_write_word_data(SHUTDOWN, 0);
}

static int ec_restart_notify(struct notifier_block *this,
			     unsigned long reboot_mode, void *data)
{
	if (reboot_mode == REBOOT_WARM)
		a500_ec_write_word_data_locked(WARM_REBOOT, 0);
	else
		a500_ec_write_word_data_locked(COLD_REBOOT, 1);

	return NOTIFY_DONE;
}

static struct notifier_block ec_restart_handler = {
	.notifier_call = ec_restart_notify,
	.priority = 200,
};

static const struct mfd_cell ec_cell[] = {
	{
		.name = "a500-battery",
		.of_compatible = "acer,a500-iconia-battery",
	},
	{
		.name = "a500-leds",
		.of_compatible = "acer,a500-iconia-leds",
	},
};

static int ec_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int err;

	ec_chip = devm_kzalloc(&client->dev, sizeof(*ec_chip), GFP_KERNEL);
	if (!ec_chip)
		return -ENOMEM;

	ec_chip->client = client;

	/* register battery and LED devices */
	err = mfd_add_devices(&client->dev, -1, ec_cell, ARRAY_SIZE(ec_cell),
			      NULL, 0, NULL);
	if (err)
		dev_err(&client->dev, "failed to add subdevices: %d\n", err);

	/* set up power management functions */
	if (of_device_is_system_power_controller(client->dev.of_node)) {
		err = register_restart_handler(&ec_restart_handler);
		if (err)
			dev_err(&client->dev,
				"unable to register restart handler: %d\n",
				err);

		if (!pm_power_off)
			pm_power_off = ec_poweroff;
	}

	return 0;
}

static const struct of_device_id ec_match[] = {
	{ .compatible = "acer,a500-iconia-ec" },
	{ }
};
MODULE_DEVICE_TABLE(of, ec_match);

static const struct i2c_device_id ec_id[] = {
	{ "a500-iconia-ec", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, ec_id);

static struct i2c_driver a500_ec_driver = {
	.driver = {
		.name = "a500-ec",
		.of_match_table = ec_match,
	},
	.id_table = ec_id,
	.probe = ec_probe,
};
builtin_i2c_driver(a500_ec_driver);

MODULE_DESCRIPTION("Acer Iconia Tab A500 Embedded Controller driver");
MODULE_AUTHOR("Dmitry Osipenko <digetx@gmail.com>");
MODULE_LICENSE("GPL v2");
