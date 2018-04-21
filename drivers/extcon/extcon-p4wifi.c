/*
 * extcon-p4wifi.c - Galaxy Tab 10.1 (p4wifi) usb path
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#define DEBUG

#include <linux/delay.h>
#include <linux/extcon-provider.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/consumer.h>
#include <linux/regulator/consumer.h>

typedef enum
{
	USB_SEL_AP_USB = 0,
	USB_SEL_CP_USB,
	USB_SEL_ADC
} p4wifi_usb_path_type;

static const unsigned int p4wifi_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_CHG_USB_FAST,
	EXTCON_CHG_USB_SLOW,
	EXTCON_NONE,
};

struct p4wifi_usb_data {
	struct device *dev;
	struct extcon_dev *edev;
	struct iio_channel *adc_channel;
	struct regulator *regulator;
	struct gpio_desc *connect_gpio;
	struct gpio_desc *usb_sel1_gpio;
	struct gpio_desc *usb_sel2_gpio;

	p4wifi_usb_path_type usb_sel_status;
};

static const char* get_path_name(p4wifi_usb_path_type usb_path)
{
	switch(usb_path) {
	case USB_SEL_AP_USB:
		return "USB_SEL_AP_USB";
	case USB_SEL_CP_USB:
		return "USB_SEL_CP_USB";
	case USB_SEL_ADC:
		return "USB_SEL_ADC";
	default:
		break;
	}
	return "UNKNOWN";
}

static void p4wifi_usb_path_set(struct p4wifi_usb_data *data,
	p4wifi_usb_path_type usb_path)
{
	if (usb_path == data->usb_sel_status) {
		dev_info(data->dev, "%s: usb_path already set to: %s\n",
			__func__, get_path_name(usb_path));
		return;
	}

	dev_info(data->dev, "%s: usb_path=%s\n", __func__,
		get_path_name(usb_path));
	if (usb_path == USB_SEL_AP_USB) {
		gpiod_set_value(data->usb_sel1_gpio, 1);
		gpiod_set_value(data->usb_sel2_gpio, 1);
		data->usb_sel_status = USB_SEL_AP_USB;
	} else if (usb_path == USB_SEL_CP_USB) {
		gpiod_set_value(data->usb_sel1_gpio, 0);
		gpiod_set_value(data->usb_sel2_gpio, 0);
		data->usb_sel_status = USB_SEL_CP_USB;
	} else if (usb_path == USB_SEL_ADC) {
		gpiod_set_value(data->usb_sel1_gpio, 0);
		gpiod_set_value(data->usb_sel2_gpio, 1);
		data->usb_sel_status = USB_SEL_ADC;
	}
}

static bool p4wifi_usb_check_type(struct p4wifi_usb_data *data)
{
	struct device *dev = data->dev;
	p4wifi_usb_path_type old_usb_sel_status;
	struct regulator *reg = data->regulator;
	int regulator_enabled;
	const int nsamples = 2;
	int count, vol_1, sum = 0;
	bool is_ta;
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	regulator_enabled = regulator_is_enabled(reg);
	if (regulator_enabled < 0) {
		dev_err(dev, "%s: error regulator_is_enabled return=%d\n",
			__func__, regulator_enabled);
		return false;
	}

	if (regulator_enabled == 0) {
		ret = regulator_enable(reg);
		if (ret != 0) {
			dev_err(dev, "%s: error enabling regulator vdd_ldo6.\n",
				__func__);
			return false;
		}
		udelay(10);
	}

	old_usb_sel_status = data->usb_sel_status;
	p4wifi_usb_path_set(data, USB_SEL_ADC);

	mdelay(100);

	for (count = 0; count < nsamples; count++) {
		int val;

		ret = iio_read_channel_raw(data->adc_channel, &val);
		if (ret < 0) {
			pr_err("%s: iio read channel failed. (%d)\n", __func__, ret);
			val = 0;
		}

		mdelay(1);
		sum += val;
	}

	vol_1 = sum / nsamples;
	dev_info(dev, "%s: samsung_charger_adc = %d\n", __func__, vol_1);

	if ((vol_1 > 800) && (vol_1 < 1800))
		is_ta = true;
	else
		is_ta = false;

	mdelay(50);

	p4wifi_usb_path_set(data, old_usb_sel_status);

	if (regulator_enabled == 0)
		regulator_disable(reg);

	dev_info(dev, "%s: is_ta = %d\n",
		__func__, is_ta);
	return is_ta;
}

static void p4wifi_update_extcon_state(struct p4wifi_usb_data *data)
{
	struct device *dev = data->dev;
	bool connected, is_ta;

	connected = gpiod_get_value(data->connect_gpio);
	is_ta = p4wifi_usb_check_type(data);

	extcon_set_state(data->edev, EXTCON_CHG_USB_FAST, connected && is_ta);
	extcon_set_state(data->edev, EXTCON_CHG_USB_SLOW, connected && !is_ta);
	extcon_set_state(data->edev, EXTCON_USB, connected);
	extcon_sync(data->edev, EXTCON_USB);

	dev_info(dev, "connected=%d, is_ta=%d\n", connected, is_ta);
}

static irqreturn_t p4wifi_usb_connect_irq_handler(int irq, void *_data)
{
	struct p4wifi_usb_data *data = _data;
	struct device *dev = data->dev;

	dev_dbg(dev, "%s\n", __func__);

	p4wifi_update_extcon_state(data);

	return IRQ_HANDLED;
}

static int p4wifi_usb_init_path(struct p4wifi_usb_data *data){
	int usbsel2;

	dev_dbg(data->dev, "%s\n", __func__);

	/* Read the initial value set by bootloader */
	gpiod_direction_input(data->usb_sel2_gpio);
	usbsel2 = gpiod_get_value(data->usb_sel2_gpio);

	dev_dbg(data->dev, "%s: usbsel2=%d\n", __func__, usbsel2);

	if (usbsel2 == 1) {
		gpiod_direction_output(data->usb_sel2_gpio, 1);
		p4wifi_usb_path_set(data, USB_SEL_AP_USB);
	} else if (usbsel2 == 0) {
		gpiod_direction_output(data->usb_sel2_gpio, 0);
		p4wifi_usb_path_set(data, USB_SEL_CP_USB);
	}

	return 0;
}

static int p4wifi_usb_init_gpios(struct platform_device *pdev,
	struct p4wifi_usb_data *data)
{
	struct device *dev = &pdev->dev;
	struct gpio_desc *gpio;
	int ret = 0;

	dev_dbg(dev, "%s\n", __func__);

#if 0
	gpio = devm_gpiod_get_optional(dev, "connect", GPIOD_IN);
	if (IS_ERR(gpio)) {
		ret = PTR_ERR(gpio);
		dev_err(dev, "cannot get connect-gpio (%d)\n", ret);
		if (ret != -ENODEV && ret != -EBUSY)
			goto done;
		ret = 0;
		gpio = NULL;
	}
	data->connect_gpio = gpio;
#endif

	gpio = devm_gpiod_get(dev, "usb-sel1", GPIOD_OUT_LOW);
	if (IS_ERR(gpio)) {
		ret = PTR_ERR(gpio);
		dev_err(dev, "cannot get usb-sel1-gpio (%d)\n", ret);
		goto done;
	}
	data->usb_sel1_gpio = gpio;

	gpio = devm_gpiod_get(dev, "usb-sel2", GPIOD_ASIS);
	if (IS_ERR(gpio)) {
		ret = PTR_ERR(gpio);
		dev_err(dev, "cannot get usb-sel2-gpio (%d)\n", ret);
		goto done;
	}
	data->usb_sel2_gpio = gpio;

done:
	return ret;
}

static int p4wifi_usb_probe(struct platform_device *pdev)
{
	struct p4wifi_usb_data *data;
	struct device *dev = &pdev->dev;
	struct regulator *regulator;
	int irq;
	int ret = 0;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->adc_channel = devm_iio_channel_get(dev, "usb-cable-detect");
	if (IS_ERR(data->adc_channel)) {
		ret = PTR_ERR(data->adc_channel);
		dev_err(dev, "Error getting usb-cable-detect ADC channel (%d)\n", ret);
		ret = -EPROBE_DEFER;
		goto err;
	}

	dev_dbg(dev, "%s +\n", __func__);

	data->usb_sel_status = -1;
	data->dev = dev;
	platform_set_drvdata(pdev, data);

	regulator = devm_regulator_get(dev, "vcc");
	if (IS_ERR(regulator)) {
		ret = PTR_ERR(regulator);
		dev_err(dev, "error getting vcc regulator (%d)\n", ret);
		return ret;
	}

	data->regulator = regulator;

	ret = p4wifi_usb_init_gpios(pdev, data);
	if (ret)
		return ret;

	if (data->connect_gpio) {
		irq = gpiod_to_irq(data->connect_gpio);
		ret = devm_request_threaded_irq(dev, irq,
				NULL, p4wifi_usb_connect_irq_handler,
				IRQF_TRIGGER_FALLING |
				IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				"p4wifi-usb-connect", data);
		if (ret) {
			dev_err(dev, "Cannot request irq %d for Fault (%d)\n", irq, ret);
			return ret;
		}

		ret = enable_irq_wake(irq);
		if (ret)
			dev_err(dev, "failed to enable_irq wake (%d)\n", ret);
	}

	data->edev = devm_extcon_dev_allocate(dev, p4wifi_extcon_cable);
	if (IS_ERR(data->edev)) {
		dev_err(dev, "failed to allocate extcon device\n");
		return -ENOMEM;
	}

	ret = devm_extcon_dev_register(dev, data->edev);
	if (ret) {
		dev_err(dev, "failed to register extcon device\n");
		return ret;
	}

	p4wifi_usb_init_path(data);
	p4wifi_update_extcon_state(data);

	dev_dbg(dev, "%s -\n", __func__);

	return 0;

err:
	return ret;
}

static const struct of_device_id p4wifi_usb_match_ids[] = {
	{ .compatible = "samsung,p4wifi-usb", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, p4wifi_usb_match_ids);

static struct platform_driver p4wifi_usb_driver = {
	.probe	= p4wifi_usb_probe,
	.driver = {
		.name	= "p4wifi-usb",
		.of_match_table = p4wifi_usb_match_ids,
	},
};
module_platform_driver(p4wifi_usb_driver);

MODULE_DESCRIPTION("Galaxy Tab 10.1 (p4wifi) USB connector driver");
MODULE_AUTHOR("ryang <decatf@gmail.com>");
MODULE_LICENSE("GPL");
