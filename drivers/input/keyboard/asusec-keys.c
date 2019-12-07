/*
 * ASUS Transformer Pad - multimedia keys
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
#include <linux/input.h>
#include <linux/mfd/asusec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define ASUSEC_EXT_KEY_CODES 0x20

struct asusec_keys_data
{
	struct notifier_block	  nb;
	const struct asusec_info *ec;
	struct input_dev	 *xidev;
	unsigned short		  keymap[ASUSEC_EXT_KEY_CODES];
};

static const unsigned short asusec_dock_ext_keys[] = {
	[0x01] = KEY_SCREENLOCK,
	[0x02] = KEY_WLAN,
	[0x03] = KEY_BLUETOOTH,
	[0x04] = KEY_TOUCHPAD_TOGGLE,
	[0x05] = KEY_BRIGHTNESSDOWN,
	[0x06] = KEY_BRIGHTNESSUP,
	[0x07] = KEY_BRIGHTNESS_AUTO,
	[0x08] = KEY_CAMERA,
	[0x10] = KEY_WWW,
	[0x11] = KEY_CONFIG,
	[0x12] = KEY_PREVIOUSSONG,
	[0x13] = KEY_PLAYPAUSE,
	[0x14] = KEY_NEXTSONG,
	[0x15] = KEY_MUTE,
	[0x16] = KEY_VOLUMEDOWN,
	[0x17] = KEY_VOLUMEUP,
};

static void asusec_keys_report_key(struct input_dev *dev, unsigned code,
				   unsigned key, bool value)
{
	input_event(dev, EV_MSC, MSC_SCAN, code);
	input_report_key(dev, key, value);
	input_sync(dev);
}

static int asusec_keys_process_key(struct input_dev *dev, u8 code)
{
	unsigned key;

	key = 0;
	if (code < dev->keycodemax) {
		unsigned short *map = dev->keycode;
		key = map[code];
	}
	if (!key)
		key = KEY_UNKNOWN;

	asusec_keys_report_key(dev, code, key, 1);
	asusec_keys_report_key(dev, code, key, 0);

	return NOTIFY_OK;
}

static int asusec_keys_notify(struct notifier_block *nb,
			      unsigned long action, void *data_)
{
	struct asusec_keys_data *priv = container_of(nb, struct asusec_keys_data, nb);
	u8 *data = data_;

	if (action & ASUSEC_SMI_MASK)
		return NOTIFY_DONE;

	if (action & ASUSEC_SCI_MASK)
		return asusec_keys_process_key(priv->xidev, data[2]);

	return NOTIFY_DONE;
}

static void asusec_keys_setup_keymap(struct asusec_keys_data *priv)
{
	struct input_dev *dev = priv->xidev;
	size_t i;

	BUILD_BUG_ON(ARRAY_SIZE(priv->keymap) < ARRAY_SIZE(asusec_dock_ext_keys));

	dev->keycode = priv->keymap;
	dev->keycodesize = sizeof(*priv->keymap);
	dev->keycodemax = ARRAY_SIZE(priv->keymap);

	input_set_capability(dev, EV_MSC, MSC_SCAN);
	input_set_capability(dev, EV_KEY, KEY_UNKNOWN);

	for (i = 0; i < ARRAY_SIZE(asusec_dock_ext_keys); ++i) {
		unsigned code = asusec_dock_ext_keys[i];
		if (!code)
			continue;

		__set_bit(code, dev->keybit);
		priv->keymap[i] = code;
	}
}

static int asusec_keys_probe(struct platform_device *dev)
{
	const struct asusec_info *ec = asusec_cell_to_ec(dev);
	struct i2c_client *parent = to_i2c_client(dev->dev.parent);
	struct asusec_keys_data *priv;
	int ret;

	priv = devm_kzalloc(&dev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(dev, priv);
	priv->ec = ec;

	priv->xidev = devm_input_allocate_device(&dev->dev);
	if (!priv->xidev)
		return -ENOMEM;

	priv->xidev->name = devm_kasprintf(&dev->dev, GFP_KERNEL,
					   "%s Keyboard Ext", ec->model);
	priv->xidev->phys = devm_kasprintf(&dev->dev, GFP_KERNEL,
					   "i2c-%u-%04x",
					   i2c_adapter_id(parent->adapter),
					   parent->addr);
	asusec_keys_setup_keymap(priv);

	ret = input_register_device(priv->xidev);
	if (ret < 0) {
		dev_err(&dev->dev, "failed to register extension keys: %d\n", ret);
		return ret;
	}

	priv->nb.notifier_call = asusec_keys_notify;
	ret = asusec_register_notifier(ec, &priv->nb);

	return ret;
}

static int asusec_keys_remove(struct platform_device *dev)
{
	struct asusec_keys_data *priv = platform_get_drvdata(dev);

	asusec_unregister_notifier(priv->ec, &priv->nb);

	return 0;
}

static struct platform_driver asusec_keys_driver = {
	.driver.name = "asusec-keys",
	.probe = asusec_keys_probe,
	.remove = asusec_keys_remove,
};

module_platform_driver(asusec_keys_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer Pad multimedia keys driver");
MODULE_LICENSE("GPL");
