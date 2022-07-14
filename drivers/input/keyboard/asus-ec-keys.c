// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ASUS Transformer Pad - multimedia keys
 */

#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/mfd/asus-ec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define ASUSEC_EXT_KEY_CODES 0x20

#define ASUSEC_TOUCHPAD_ON   0xF4D4
#define ASUSEC_TOUCHPAD_OFF  0xF5D4

struct asusec_keys_data {
	struct notifier_block nb;
	struct asusec_info *ec;
	struct input_dev *xidev;
	bool special_key_pressed;
	bool special_key_mode;
	bool touchpad_enabled;
	unsigned short keymap[ASUSEC_EXT_KEY_CODES * 2];
};

static void asusec_input_event(struct input_handle *handle,
			       unsigned int event_type,
			       unsigned int event_code, int value)
{
	struct asusec_keys_data *priv = handle->handler->private;

	/* Store special key state */
	if (event_type == EV_KEY && event_code == KEY_RIGHTALT)
		priv->special_key_pressed = !!value;
}

static int asusec_input_connect(struct input_handler *handler,
				struct input_dev *dev,
				const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "asusec-media-handler";

	error = input_register_handle(handle);
	if (error)
		goto err_free_handle;

	error = input_open_device(handle);
	if (error)
		goto err_unregister_handle;

	return 0;

 err_unregister_handle:
	input_unregister_handle(handle);
 err_free_handle:
	kfree(handle);
	return error;
}

static void asusec_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id asusec_input_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ }
};

static struct input_handler asusec_input_handler = {
	.name =	"asusec-media-handler",
	.event = asusec_input_event,
	.connect = asusec_input_connect,
	.disconnect = asusec_input_disconnect,
	.id_table = asusec_input_ids,
};

static const unsigned short asusec_dock_ext_keys[] = {
	/* Function keys [0x00 - 0x19] */
	[0x01] = KEY_DELETE,
	[0x02] = KEY_F1,
	[0x03] = KEY_F2,
	[0x04] = KEY_F3,
	[0x05] = KEY_F4,
	[0x06] = KEY_F5,
	[0x07] = KEY_F6,
	[0x08] = KEY_F7,
	[0x10] = KEY_F8,
	[0x11] = KEY_F9,
	[0x12] = KEY_F10,
	[0x13] = KEY_F11,
	[0x14] = KEY_F12,
	[0x15] = KEY_MUTE,
	[0x16] = KEY_VOLUMEDOWN,
	[0x17] = KEY_VOLUMEUP,
	/* Multimedia keys [0x20 - 0x39] */
	[0x21] = KEY_SCREENLOCK,
	[0x22] = KEY_WLAN,
	[0x23] = KEY_BLUETOOTH,
	[0x24] = KEY_TOUCHPAD_TOGGLE,
	[0x25] = KEY_BRIGHTNESSDOWN,
	[0x26] = KEY_BRIGHTNESSUP,
	[0x27] = KEY_BRIGHTNESS_AUTO,
	[0x28] = KEY_CAMERA,
	[0x30] = KEY_WWW,
	[0x31] = KEY_CONFIG,
	[0x32] = KEY_PREVIOUSSONG,
	[0x33] = KEY_PLAYPAUSE,
	[0x34] = KEY_NEXTSONG,
	[0x35] = KEY_MUTE,
	[0x36] = KEY_VOLUMEDOWN,
	[0x37] = KEY_VOLUMEUP,
};

static void asusec_keys_report_key(struct input_dev *dev, unsigned int code,
				   unsigned int key, bool value)
{
	input_event(dev, EV_MSC, MSC_SCAN, code);
	input_report_key(dev, key, value);
	input_sync(dev);
}

static int asusec_keys_process_key(struct input_dev *dev, u8 code)
{
	struct asusec_keys_data *priv = dev_get_drvdata(dev->dev.parent);
	unsigned int key = 0;

	if (code == 0)
		return NOTIFY_DONE;

	/* Flip special key mode state when pressing key 1 with special key pressed */
	if (priv->special_key_pressed && code == 1) {
		priv->special_key_mode = !priv->special_key_mode;
		return NOTIFY_DONE;
	}

	/*
	 * Relocate code to second "page" if pressed state XOR's mode state
	 * This way special key will invert the current mode
	 */
	if (priv->special_key_mode ^ priv->special_key_pressed)
		code += ASUSEC_EXT_KEY_CODES;

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
	unsigned int i;

	BUILD_BUG_ON(ARRAY_SIZE(priv->keymap) < ARRAY_SIZE(asusec_dock_ext_keys));

	dev->keycode = priv->keymap;
	dev->keycodesize = sizeof(*priv->keymap);
	dev->keycodemax = ARRAY_SIZE(priv->keymap);

	input_set_capability(dev, EV_MSC, MSC_SCAN);
	input_set_capability(dev, EV_KEY, KEY_UNKNOWN);

	for (i = 0; i < ARRAY_SIZE(asusec_dock_ext_keys); i++) {
		unsigned int code = asusec_dock_ext_keys[i];

		if (!code)
			continue;

		__set_bit(code, dev->keybit);
		priv->keymap[i] = code;
	}
}

static ssize_t touchpad_show(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	struct asusec_keys_data *priv = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", priv->touchpad_enabled);
}

static ssize_t touchpad_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct asusec_keys_data *priv = dev_get_drvdata(dev);
	int state;

	sscanf(buf, "%d", &state);

	/* Set the Touchpad switch: 0 - Disable, 1 - Enable */
	if (state) {
		state = asusec_i2c_command(priv->ec, ASUSEC_TOUCHPAD_ON);
		if (!state)
			priv->touchpad_enabled = true;
	} else {
		state = asusec_i2c_command(priv->ec, ASUSEC_TOUCHPAD_OFF);
		if (!state)
			priv->touchpad_enabled = false;
	}

	return count;
}

static DEVICE_ATTR_RW(touchpad);

static struct attribute *asusec_keys_attributes[] = {
	&dev_attr_touchpad.attr,
	NULL
};

static const struct attribute_group asusec_keys_attr_group = {
	.attrs = asusec_keys_attributes,
};

static int asusec_keys_probe(struct platform_device *pdev)
{
	struct asusec_info *ec = asusec_cell_to_ec(pdev);
	struct i2c_client *parent = to_i2c_client(pdev->dev.parent);
	struct asusec_keys_data *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->ec = ec;

	priv->xidev = devm_input_allocate_device(&pdev->dev);
	if (!priv->xidev)
		return -ENOMEM;

	priv->nb.notifier_call = asusec_keys_notify;

	priv->xidev->name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					   "%s Keyboard Ext", ec->model);
	priv->xidev->phys = devm_kasprintf(&pdev->dev, GFP_KERNEL,
					   "i2c-%u-%04x",
					   i2c_adapter_id(parent->adapter),
					   parent->addr);
	asusec_keys_setup_keymap(priv);

	ret = input_register_device(priv->xidev);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register extension keys: %d\n",
			ret);
		return ret;
	}

	ret = asusec_i2c_command(priv->ec, ASUSEC_TOUCHPAD_ON);	
	if (!ret)
		priv->touchpad_enabled = true;

	ret = devm_device_add_group(&pdev->dev, &asusec_keys_attr_group);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to create sysfs attributes\n");

	asusec_input_handler.private = priv;

	ret = input_register_handler(&asusec_input_handler);
	if (ret)
		return ret;

	ret = asusec_register_notifier(ec, &priv->nb);
	if (ret)
		input_unregister_handler(&asusec_input_handler);

	return ret;
}

static int asusec_keys_remove(struct platform_device *pdev)
{
	struct asusec_keys_data *priv = platform_get_drvdata(pdev);

	asusec_unregister_notifier(priv->ec, &priv->nb);

	input_unregister_handler(&asusec_input_handler);

	return 0;
}

static struct platform_driver asusec_keys_driver = {
	.driver.name = "asusec-keys",
	.probe = asusec_keys_probe,
	.remove = asusec_keys_remove,
};
module_platform_driver(asusec_keys_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer's multimedia keys driver");
MODULE_LICENSE("GPL");
