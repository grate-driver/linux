/*
 * ASUS EC - keyboard and touchpad
 *
 * This looks suspiciously similar to i8042, but wrapped in
 * I2C/SMBus packets.
 *
 * Written by: Michał Mirosław <mirq-linux@rere.qmqm.pl>
 *
 * Copyright (C) 2017 Michał Mirosław
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/i2c.h>
#include <linux/i8042.h>
#include <linux/mfd/asusec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/serio.h>
#include <linux/slab.h>

struct asusec_kbc_data
{
	struct notifier_block	  nb;
	const struct asusec_info *ec;
	struct serio		 *sdev[2];
};

static int asusec_kbc_notify(struct notifier_block *nb,
			     unsigned long action, void *data_)
{
	struct asusec_kbc_data *priv = container_of(nb, struct asusec_kbc_data, nb);
	struct serio *port = NULL;
	unsigned n;
	u8 *data = data_;

	if (action & (ASUSEC_SMI_MASK|ASUSEC_SCI_MASK))
		return NOTIFY_DONE;
	else if (action & ASUSEC_AUX_MASK)
		port = priv->sdev[1];
	else if (action & (ASUSEC_KBC_MASK|ASUSEC_KEY_MASK))
		port = priv->sdev[0];
	else
		return NOTIFY_DONE;

	n = data[0] - 1;
	data += 2;
	while (n--)
		serio_interrupt(port, *data++, 0);

	return NOTIFY_OK;
}

static int asusec_serio_write(struct serio *port, unsigned char data)
{
	const struct asusec_info *ec = port->port_data;

	return asusec_i2c_command(ec, (data << 8) | port->id.extra);
}

static int asusec_register_serio(struct platform_device *dev, int idx,
				 const char *name, int cmd)
{
	struct asusec_kbc_data *priv = platform_get_drvdata(dev);
	struct i2c_client *parent = to_i2c_client(dev->dev.parent);
	struct serio *port = kzalloc(sizeof(*port), GFP_KERNEL);

	if (!port) {
		dev_err(&dev->dev, "No memory for serio%d\n", idx);
		return -ENOMEM;
	}

	priv->sdev[idx] = port;
	port->dev.parent = &dev->dev;
	port->id.type = SERIO_8042;
	port->id.extra = cmd & 0xFF;
	port->write = asusec_serio_write;
	port->port_data = (void *)priv->ec;
	snprintf(port->name, sizeof(port->name), "%s %s",
		 priv->ec->model, name);
	snprintf(port->phys, sizeof(port->phys), "i2c-%u-%04x/serio%d",
		 i2c_adapter_id(parent->adapter), parent->addr, idx);

	serio_register_port(port);

	return 0;
}

static int asusec_kbc_probe(struct platform_device *dev)
{
	const struct asusec_info *ec = asusec_cell_to_ec(dev);
	struct asusec_kbc_data *priv;
	int ret;

	priv = devm_kzalloc(&dev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(dev, priv);
	priv->ec = ec;

	ret = asusec_register_serio(dev, 0, "Keyboard", 0);
	if (ret < 0)
		return ret;

	ret = asusec_register_serio(dev, 1, "Touchpad", I8042_CMD_AUX_SEND);
	if (ret < 0) {
		serio_unregister_port(priv->sdev[0]);
		return ret;
	}

	priv->nb.notifier_call = asusec_kbc_notify;
	ret = asusec_register_notifier(ec, &priv->nb);

	return ret;
}

static int asusec_kbc_remove(struct platform_device *dev)
{
	struct asusec_kbc_data *priv = platform_get_drvdata(dev);

	asusec_unregister_notifier(priv->ec, &priv->nb);
	serio_unregister_port(priv->sdev[1]);
	serio_unregister_port(priv->sdev[0]);

	return 0;
}

static struct platform_driver asusec_kbc_driver = {
	.driver.name = "asusec-kbc",
	.probe = asusec_kbc_probe,
	.remove = asusec_kbc_remove,
};

module_platform_driver(asusec_kbc_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer Pad Dock keyboard+touchpad controller driver");
MODULE_LICENSE("GPL");
