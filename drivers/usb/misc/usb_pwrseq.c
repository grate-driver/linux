// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct usb_pwrseq_data {
	struct device *dev;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *power_gpio;
};

static int usb_pwrseq_probe(struct platform_device *pdev)
{
	struct usb_pwrseq_data *data;
	struct device *dev = &pdev->dev;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(data->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(data->reset_gpio),
					 "failed to get reset GPIO\n");

	if (data->reset_gpio) {
		msleep(1);
		gpiod_set_value(data->reset_gpio, 0);
		msleep(100);
	}

	data->power_gpio = devm_gpiod_get(dev, "power", GPIOD_OUT_HIGH);
	if (IS_ERR(data->power_gpio))
		return dev_err_probe(dev, PTR_ERR(data->power_gpio),
					 "failed to get power GPIO\n");

	return 0;
}

static const struct of_device_id usb_pwrseq_of_match[] = {
	{ .compatible = "usb457,817", },
	{ .compatible = "usb4f2,b354", },
	{},
};
MODULE_DEVICE_TABLE(of, usb_pwrseq_of_match);

static struct platform_driver usb_pwrseq_driver = {
	.probe = usb_pwrseq_probe,
	.driver = {
		.name = "usb-pwrseq",
		.of_match_table = usb_pwrseq_of_match,
	},
};
module_platform_driver(usb_pwrseq_driver);

MODULE_LICENSE("GPL");
