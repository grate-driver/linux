// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASUS EC driver
 *
 * Written by: Michał Mirosław <mirq-linux@rere.qmqm.pl>
 *
 * Copyright (C) 2017 Michał Mirosław
 *
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/mfd/asus-ec.h>

#define ASUSEC_SMI_HANDSHAKE		0x50
#define ASUSEC_SMI_RESET		0x5F

#define ASUSEC_CTL_SUSB_MODE		BIT_ULL(0x09)
#define ASUSEC_CTL_FACTORY_MODE		BIT_ULL(0x26)

#define RSP_BUFFER_SIZE			8

struct asus_ec_data {
	struct asusec_info info;
	struct mutex ecreq_lock;
	struct gpio_desc *ecreq;
	struct i2c_client *self;
	u8 ec_data[DOCKRAM_ENTRY_BUFSIZE];
	bool logging_disabled;
};

#define to_ec_data(ec) \
	container_of(ec, struct asus_ec_data, info)

enum asus_ec_subdev_id {
	ID_EC_PART_BATTERY,
#define EC_PART_BATTERY BIT(ID_EC_PART_BATTERY)

	ID_EC_PART_CHARGE_LED,
#define EC_PART_CHARGE_LED BIT(ID_EC_PART_CHARGE_LED)

	ID_EC_PART_I8042,
#define EC_PART_I8042 BIT(ID_EC_PART_I8042)

	ID_EC_PART_EXT_KEYS,
#define EC_PART_EXT_KEYS BIT(ID_EC_PART_EXT_KEYS)

	ID_EC_PART_CHARGER,
#define EC_PART_CHARGER BIT(ID_EC_PART_CHARGER)
};

enum asus_ec_flag {
	EC_FLAGBIT_SET_MODE,
#define EC_FLAG_SET_MODE BIT(EC_FLAGBIT_SET_MODE)
};

struct asus_ec_initdata {
	const char *model;
	const char *name;
	unsigned int components;
	unsigned int flags;
};

static struct asusec_platform_data asusec_pdata = {
	.battery_addr = 0x14,
	.charger_addr = 0x0A,
};

static const struct mfd_cell asus_ec_subdev[] = {
	[ID_EC_PART_BATTERY] = {
		.name = "asusec-battery",
		.platform_data = &asusec_pdata,
		.pdata_size = sizeof(asusec_pdata),
	},
	[ID_EC_PART_CHARGE_LED] = {
		.name = "asusec-led",
	},
	[ID_EC_PART_I8042] = {
		.name = "asusec-kbc",
	},
	[ID_EC_PART_EXT_KEYS] = {
		.name = "asusec-keys",
	},
	[ID_EC_PART_CHARGER] = {
		.name = "asusec-charger",
		.platform_data = &asusec_pdata,
		.pdata_size = sizeof(asusec_pdata),
	},
};

static const struct asus_ec_initdata asus_ec_model_info[] = {
	{	/* Asus T20 Mobile Dock  */
		.model		= "ASUS-EP101-DOCK",
		.name		= "dock",
		.components	= EC_PART_BATTERY | EC_PART_CHARGE_LED |
				  EC_PART_I8042 | EC_PART_EXT_KEYS |
				  EC_PART_CHARGER,
	},
	{	/* Asus T30 Transformer Pad */
		.model		= "ASUS-TF201-PAD",
		.name		= "pad",
		.components	= EC_PART_BATTERY | EC_PART_CHARGE_LED,
		.flags		= EC_FLAG_SET_MODE,
	},
	{	/* Asus T30 Mobile Dock */
		.model		= "ASUS-TF201-DOCK",
		.name		= "dock",
		.components	= EC_PART_BATTERY | EC_PART_CHARGE_LED |
				  EC_PART_I8042 | EC_PART_EXT_KEYS |
				  EC_PART_CHARGER,
	},
	{	/* Asus TF500T/TF700T Mobile Dock */
		.model		= "ASUS-DOCK-EC21N",
		.name		= "dock",
		.components	= EC_PART_BATTERY | EC_PART_CHARGE_LED |
				  EC_PART_I8042 | EC_PART_EXT_KEYS |
				  EC_PART_CHARGER,
	},
	{	/* Asus T114 Transformer Pad */
		.model		= "ASUS-TF701T-PAD",
		.name		= "pad",
		.components	= EC_PART_BATTERY | EC_PART_CHARGE_LED,
		.flags		= EC_FLAG_SET_MODE,
	},
};

static int asusec_signal_request(const struct asusec_info *ec)
{
	struct asus_ec_data *priv = to_ec_data(ec);

	mutex_lock(&priv->ecreq_lock);

	dev_dbg(&priv->self->dev, "EC request\n");

	gpiod_set_value_cansleep(priv->ecreq, 1);
	msleep(50);

	gpiod_set_value_cansleep(priv->ecreq, 0);
	msleep(200);

	mutex_unlock(&priv->ecreq_lock);

	return 0;
}

static int asus_ec_write(struct asus_ec_data *priv, u16 data)
{
	int ret = i2c_smbus_write_word_data(priv->self, 0x64, data);

	dev_dbg(&priv->self->dev, "EC write: %04x, ret = %d\n", data, ret);
	return ret;
}

static int asus_ec_read(struct asus_ec_data *priv, bool in_irq)
{
	int ret = i2c_smbus_read_i2c_block_data(priv->self, 0x6A, 8, priv->ec_data);

	dev_dbg(&priv->self->dev, "EC read: %*ph, ret = %d%s\n", 8, priv->ec_data,
		ret, in_irq ? "; in irq" : "");
	return ret;
}

int asusec_i2c_command(const struct asusec_info *ec, u16 data)
{
	return asus_ec_write(to_ec_data(ec), data);
}
EXPORT_SYMBOL_GPL(asusec_i2c_command);

static void asus_ec_clear_buffer(struct asus_ec_data *priv)
{
	int retry = RSP_BUFFER_SIZE;

	while (retry--) {
		if (asus_ec_read(priv, false) < 0)
			continue;

		if (priv->ec_data[1] & ASUSEC_OBF_MASK)
			continue;

		break;
	}
}

static int asus_ec_log_info(struct asus_ec_data *priv, unsigned int reg,
			    const char *name, char **out)
{
	char *buf = priv->ec_data;
	int ret;

	ret = asus_dockram_read(priv->info.dockram, reg, buf);
	if (ret < 0)
		return ret;

	if (!priv->logging_disabled)
		dev_info(&priv->self->dev, "%-14s: %.*s\n", name, buf[0], buf + 1);

	if (out)
		*out = kstrndup(buf + 1, buf[0], GFP_KERNEL);

	return 0;
}

static int asus_ec_reset(struct asus_ec_data *priv)
{
	int retry, ret;

	for (retry = 0; retry < 3; ++retry) {
		ret = asus_ec_write(priv, 0);
		if (!ret)
			return 0;

		msleep(300);
	}

	return ret;
}

static int asus_ec_magic_debug(struct asus_ec_data *priv)
{
	u64 flag;
	int ret;

	ret = asusec_get_ctl(&priv->info, &flag);
	if (ret < 0)
		return ret;

	flag &= ASUSEC_CTL_SUSB_MODE;
	dev_info(&priv->self->dev, "EC FW behaviour: %s\n",
		 flag ? "susb on when receive ec_req" : "susb on when system wakeup");

	return 0;
}

/* asuspec: normal mode */
static int asus_ec_enter_normal_mode(struct asus_ec_data *priv)
{
	dev_dbg(&priv->self->dev, "Entering normal mode.\n");
	return asusec_clear_ctl_bits(&priv->info, ASUSEC_CTL_FACTORY_MODE);
}

static void asus_ec_handle_smi(struct asus_ec_data *priv, unsigned int code);

static irqreturn_t asus_ec_interrupt(int irq, void *dev_id)
{
	struct asus_ec_data *priv = dev_id;
	int ret;

	ret = asus_ec_read(priv, true);

	if (ret > 1 && priv->ec_data[1] & ASUSEC_OBF_MASK) {
		if (priv->ec_data[1] & ASUSEC_SMI_MASK)
			asus_ec_handle_smi(priv, priv->ec_data[2]);

		blocking_notifier_call_chain(&priv->info.notify_list,
					     priv->ec_data[1],
					     priv->ec_data);

		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static const struct asus_ec_initdata *asus_ec_of_match(const char *model)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(asus_ec_model_info); i++) {
		if (!strcmp(model, asus_ec_model_info[i].model))
			return &asus_ec_model_info[i];
	}

	return NULL;
}

static int asus_ec_init_components(struct asus_ec_data *priv,
				   const struct asus_ec_initdata *info)
{
	struct mfd_cell *selected_cells;
	unsigned int n, i;
	int ret;

	n = hweight8(info->components);
	selected_cells = kcalloc(n, sizeof(*selected_cells), GFP_KERNEL);
	if (!selected_cells)
		return -ENOMEM;

	for (i = 0, n = 0; i < ARRAY_SIZE(asus_ec_subdev); i++) {
		if (~info->components & BIT(i))
			continue;

		memcpy(&selected_cells[n++], &asus_ec_subdev[i],
		       sizeof(*selected_cells));
	}

	ret = mfd_add_devices(&priv->self->dev, PLATFORM_DEVID_AUTO,
			      selected_cells, n, NULL, 0, NULL);
	if (ret)
		dev_err(&priv->self->dev, "failed to add subdevs: %d\n", ret);

	kfree(selected_cells);
	return ret;
}

static const struct asus_ec_initdata *asus_ec_detect(struct asus_ec_data *priv)
{
	const struct asus_ec_initdata *info;
	char *model = NULL;
	int ret;

	ret = asus_ec_reset(priv);
	if (ret)
		goto err_exit;

	asus_ec_clear_buffer(priv);

	ret = asus_ec_log_info(priv, 0x01, "model", &model);
	if (ret)
		goto err_exit;

	ret = asus_ec_log_info(priv, 0x02, "FW version", NULL);
	if (ret)
		goto err_exit;

	ret = asus_ec_log_info(priv, 0x03, "Config format", NULL);
	if (ret)
		goto err_exit;

	ret = asus_ec_log_info(priv, 0x04, "HW version", NULL);
	if (ret)
		goto err_exit;

	priv->logging_disabled = true;

	ret = asus_ec_magic_debug(priv);
	if (ret)
		goto err_exit;

	info = asus_ec_of_match(model);
	if (!info) {
		dev_err(&priv->self->dev, "EC model not recognized\n");
		ret = -ENODEV;
		goto out_free;
	}

	priv->info.name = info->name;
	priv->info.model = info->model;

	if (info->flags & EC_FLAG_SET_MODE)
		asus_ec_enter_normal_mode(priv);

	kfree(model);

	return info;

err_exit:
	if (ret)
		dev_err(&priv->self->dev, "failed to access EC: %d\n", ret);
out_free:
	kfree(model);
	return ERR_PTR(ret);
}

static void asus_ec_handle_smi(struct asus_ec_data *priv, unsigned int code)
{
	dev_dbg(&priv->self->dev, "SMI interrupt: 0x%02x\n", code);

	switch (code) {
	case ASUSEC_SMI_HANDSHAKE:
	case ASUSEC_SMI_RESET:
		asus_ec_detect(priv);
		break;
	}
}

static ssize_t ec_request_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct asusec_info *ec = dev_get_drvdata(dev);

	asusec_signal_request(ec);

	return count;
}

static ssize_t ec_irq_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct asusec_info *ec = dev_get_drvdata(dev);
	struct asus_ec_data *priv = to_ec_data(ec);

	irq_wake_thread(priv->self->irq, priv);

	return count;
}

static DEVICE_ATTR_WO(ec_request);
static DEVICE_ATTR_WO(ec_irq);

static struct attribute *asus_ec_attributes[] = {
	&dev_attr_ec_request.attr,
	&dev_attr_ec_irq.attr,
	NULL
};

static const struct attribute_group asus_ec_attr_group = {
	.attrs = asus_ec_attributes,
};

static int asus_ec_probe(struct i2c_client *client)
{
	const struct asus_ec_initdata *info;
	struct asus_ec_data *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	i2c_set_clientdata(client, &priv->info);
	priv->self = client;

	priv->info.dockram = devm_asus_dockram_get(&client->dev);
	if (IS_ERR(priv->info.dockram))
		return PTR_ERR(priv->info.dockram);

	priv->ecreq = devm_gpiod_get(&client->dev, "request", GPIOD_OUT_LOW);
	if (IS_ERR(priv->ecreq))
		return PTR_ERR(priv->ecreq);

	BLOCKING_INIT_NOTIFIER_HEAD(&priv->info.notify_list);
	mutex_init(&priv->ecreq_lock);

	ret = sysfs_create_group(&client->dev.kobj, &asus_ec_attr_group);
	if (ret)
		return ret;

	ret = sysfs_create_link(&client->dev.kobj,
				&priv->info.dockram->dev.kobj,
				"dockram");
	if (ret)
		goto unwind_sysfs_grp;

	asusec_signal_request(&priv->info);

	info = asus_ec_detect(priv);
	if (IS_ERR(info)) {
		ret = PTR_ERR(info);
		goto unwind_sysfs;
	}

	ret = devm_request_threaded_irq(&priv->self->dev, priv->self->irq,
					NULL, &asus_ec_interrupt,
					IRQF_ONESHOT | IRQF_SHARED,
					priv->self->name, priv);
	if (ret) {
		dev_err(&priv->self->dev, "failed to register IRQ %d: %d\n",
			priv->self->irq, ret);
		goto unwind_sysfs;
	}

	ret = asus_ec_init_components(priv, info);
	if (ret)
		goto unwind_sysfs;

	return 0;

unwind_sysfs:
	sysfs_remove_link(&client->dev.kobj, "dockram");
unwind_sysfs_grp:
	sysfs_remove_group(&client->dev.kobj, &asus_ec_attr_group);
	return ret;
}

static int asus_ec_remove(struct i2c_client *client)
{
	struct asus_ec_data *priv = i2c_get_clientdata(client);

	mfd_remove_devices(&priv->self->dev);

	sysfs_remove_link(&client->dev.kobj, "dockram");
	sysfs_remove_group(&client->dev.kobj, &asus_ec_attr_group);

	return 0;
}

static const struct of_device_id asus_ec_match[] = {
	{ .compatible = "asus,ec" },
	{ }
};
MODULE_DEVICE_TABLE(of, asus_ec_match);

static struct i2c_driver asus_ec_driver = {
	.driver	= {
		.name = "asus-ec",
		.of_match_table = asus_ec_match,
	},
	.probe_new = asus_ec_probe,
	.remove = asus_ec_remove,
};
module_i2c_driver(asus_ec_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer Pad's EC driver");
MODULE_LICENSE("GPL");
