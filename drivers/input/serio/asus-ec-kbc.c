// SPDX-License-Identifier: GPL-2.0-only
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
 */

#include <linux/i2c.h>
#include <linux/i8042.h>
#include <linux/mfd/asus-ec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/serio.h>
#include <linux/slab.h>

struct asusec_kbc_data {
	struct notifier_block nb;
	struct asusec_info *ec;
	struct serio *sdev[2];
};

static int asusec_kbc_notify(struct notifier_block *nb,
			     unsigned long action, void *data_)
{
	struct asusec_kbc_data *priv = container_of(nb, struct asusec_kbc_data, nb);
	unsigned int port_idx;
	unsigned int n;
	u8 *data = data_;

	if (action & (ASUSEC_SMI_MASK | ASUSEC_SCI_MASK))
		return NOTIFY_DONE;
	else if (action & ASUSEC_AUX_MASK)
		port_idx = 1;
	else if (action & (ASUSEC_KBC_MASK | ASUSEC_KEY_MASK))
		port_idx = 0;
	else
		return NOTIFY_DONE;

	n = data[0] - 1;
	data += 2;

	/*
	 * We need to replace these incoming data for keys:
	 * RIGHT_META Press   0xE0 0x27      -> LEFT_ALT   Press   0x11
	 * RIGHT_META Release 0xE0 0xF0 0x27 -> LEFT_ALT   Release 0xF0 0x11
	 * COMPOSE    Press   0xE0 0x2F      -> RIGHT_META Press   0xE0 0x27
	 * COMPOSE    Release 0xE0 0xF0 0x2F -> RIGHT_META Release 0xE0 0xF0 0x27
	 */

	if (port_idx == 0 && n >= 2 && data[0] == 0xE0) {
		if (n == 3 && data[1] == 0xF0) {
			switch (data[2]) {
			case 0x27:
				data[0] = 0xF0;
				data[1] = 0x11;
				n = 2;
				break;
			case 0x2F:
				data[2] = 0x27;
				break;
			}
		} else if (n == 2) {
			switch (data[1]) {
			case 0x27:
				data[0] = 0x11;
				n = 1;
				break;
			case 0x2F:
				data[1] = 0x27;
				break;
			}
		}
	}

	while (n--)
		serio_interrupt(priv->sdev[port_idx], *data++, 0);

	return NOTIFY_OK;
}

static int asusec_serio_write(struct serio *port, unsigned char data)
{
	const struct asusec_info *ec = port->port_data;

	return asusec_i2c_command(ec, (data << 8) | port->id.extra);
}

static int asusec_register_serio(struct device *dev, int idx,
				 const char *name, int cmd)
{
	struct asusec_kbc_data *priv = dev_get_drvdata(dev);
	struct i2c_client *parent = to_i2c_client(dev->parent);
	struct serio *port = kzalloc(sizeof(*port), GFP_KERNEL);

	if (!port)
		return -ENOMEM;

	priv->sdev[idx] = port;
	port->dev.parent = dev;
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

static int asusec_kbc_probe(struct platform_device *pdev)
{
	struct asusec_info *ec = asusec_cell_to_ec(pdev);
	struct asusec_kbc_data *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->ec = ec;

	ret = asusec_register_serio(&pdev->dev, 0, "Keyboard", 0);
	if (ret < 0)
		return ret;

	ret = asusec_register_serio(&pdev->dev, 1, "Touchpad",
				    I8042_CMD_AUX_SEND);
	if (ret < 0) {
		serio_unregister_port(priv->sdev[0]);
		return ret;
	}

	priv->nb.notifier_call = asusec_kbc_notify;
	ret = asusec_register_notifier(ec, &priv->nb);

	return ret;
}

static int asusec_kbc_remove(struct platform_device *pdev)
{
	struct asusec_kbc_data *priv = platform_get_drvdata(pdev);

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
