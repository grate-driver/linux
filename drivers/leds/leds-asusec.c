/*
 * ASUS EC driver - battery LED
 *
 * Written by: Michał Mirosław <mirq-linux@rere.qmqm.pl>
 *
 * Copyright (C) 2017 Michał Mirosław
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/leds.h>
#include <linux/mfd/asusec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define ASUSEC_CTL_LED_BLINK		(1ull << 0x28)		// 5.0
#define ASUSEC_CTL_LED_ORANGE_ON	(1ull << 0x29)		// 5.1
#define ASUSEC_CTL_LED_GREEN_ON		(1ull << 0x2A)		// 5.2
#define ASUSEC_CTL_LED_TEST_MASK	(7ull << 0x28)

static void asusec_led_set_brightness(struct led_classdev *led,
				      enum led_brightness brightness)
{
	const struct asusec_info *ec = dev_get_drvdata(led->dev->parent);

	if (brightness > 7)
		brightness = 0;

	// F[5] & 0x07
	//  auto: brightness == 0
	//  bit 0: blink / charger on?
	//  bit 1: orange on
	//  bit 2: green on
	asusec_update_ctl(ec, ASUSEC_CTL_LED_TEST_MASK,
			 (u64)brightness << 0x28);
}

static int asusec_led_probe(struct platform_device *dev)
{
	const struct asusec_info *ec = asusec_cell_to_ec(dev);
	struct led_classdev *led;
	int ret;

	platform_set_drvdata(dev, (void *)ec);

	led = devm_kzalloc(&dev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	led->name = devm_kasprintf(&dev->dev, GFP_KERNEL,
				   "%s_battery::charging", ec->name);
	led->default_trigger = "default-off";
	led->brightness_set = asusec_led_set_brightness;

	ret = devm_led_classdev_register(&dev->dev, led);
	if (ret)
		dev_err(&dev->dev, "can't register LED: %d", ret);

	return ret;
}

static struct platform_driver asusec_led_driver = {
	.driver.name = "leds-asusec",
	.probe = asusec_led_probe,
};

module_platform_driver(asusec_led_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer's charging LED driver");
MODULE_LICENSE("GPL");
