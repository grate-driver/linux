// SPDX-License-Identifier: GPL-2.0-only
/*
 * Toggles a GPIO pin to power down a device
 *
 * Jamie Lentin <jm@lentin.co.uk>
 * Andrew Lunn <andrew@lunn.ch>
 *
 * Copyright (C) 2012 Jamie Lentin
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/of_platform.h>
#include <linux/module.h>
#include <linux/reboot.h>

#define DEFAULT_TIMEOUT_MS 3000

static void gpio_poweroff_do_poweroff(void *dev)
{
	struct gpio_desc *reset_gpio = dev_get_drvdata(dev);
	u32 timeout = DEFAULT_TIMEOUT_MS;
	u32 inactive_delay = 100;
	u32 active_delay = 100;

	device_property_read_u32(dev, "inactive-delay-ms", &inactive_delay);
	device_property_read_u32(dev, "active-delay-ms", &active_delay);
	device_property_read_u32(dev, "timeout-ms", &timeout);

	/* drive it active, also inactive->active edge */
	gpiod_direction_output(reset_gpio, 1);
	mdelay(active_delay);

	/* drive inactive, also active->inactive edge */
	gpiod_set_value_cansleep(reset_gpio, 0);
	mdelay(inactive_delay);

	/* drive it active, also inactive->active edge */
	gpiod_set_value_cansleep(reset_gpio, 1);

	/* give it some time */
	mdelay(timeout);

	WARN_ON(1);
}

static int gpio_poweroff_probe(struct platform_device *pdev)
{
	struct gpio_desc *reset_gpio;
	enum gpiod_flags flags;
	bool input = false;

	input = device_property_read_bool(&pdev->dev, "input");
	if (input)
		flags = GPIOD_IN;
	else
		flags = GPIOD_OUT_LOW;

	reset_gpio = devm_gpiod_get(&pdev->dev, NULL, flags);
	if (IS_ERR(reset_gpio))
		return PTR_ERR(reset_gpio);

	platform_set_drvdata(pdev, reset_gpio);

	return devm_register_simple_power_off_handler(&pdev->dev,
						      gpio_poweroff_do_poweroff,
						      &pdev->dev);
}

static const struct of_device_id of_gpio_poweroff_match[] = {
	{ .compatible = "gpio-poweroff", },
	{},
};
MODULE_DEVICE_TABLE(of, of_gpio_poweroff_match);

static struct platform_driver gpio_poweroff_driver = {
	.probe = gpio_poweroff_probe,
	.driver = {
		.name = "poweroff-gpio",
		.of_match_table = of_gpio_poweroff_match,
	},
};

module_platform_driver(gpio_poweroff_driver);

MODULE_AUTHOR("Jamie Lentin <jm@lentin.co.uk>");
MODULE_DESCRIPTION("GPIO poweroff driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:poweroff-gpio");
