// SPDX-License-Identifier: GPL-2.0-only
/*
 * ASUS EC driver - battery LED
 *
 * Written by: Michał Mirosław <mirq-linux@rere.qmqm.pl>
 *
 * Copyright (C) 2017 Michał Mirosław
 *
 */

#include <linux/leds.h>
#include <linux/mfd/asus-ec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define ASUSEC_CTL_LED_TEST_MASK	(7ull << 0x28)

static void asusec_led_set_brightness(struct led_classdev *led,
				      enum led_brightness brightness)
{
	const struct asusec_info *ec = dev_get_drvdata(led->dev->parent);
	char led_data[DOCKRAM_ENTRY_BUFSIZE];
	char *buf = led_data;
	int ret;

	if (brightness > 7)
		brightness = 0;

	ret = asus_dockram_read(ec->dockram, 0x0A, buf);
	if (ret < 0)
		return;

	/*
	 * F[5] & 0x07
	 *  auto: brightness == 0
	 *  bit 0: blink / charger on
	 *  bit 1: orange on
	 *  bit 2: green on
	 */
	asusec_update_ctl(ec, ASUSEC_CTL_LED_TEST_MASK,
			  (u64)brightness << 0x28);
}

static int asusec_led_probe(struct platform_device *pdev)
{
	struct asusec_info *ec = asusec_cell_to_ec(pdev);
	struct led_classdev *led;
	int ret;

	platform_set_drvdata(pdev, ec);

	led = devm_kzalloc(&pdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	led->name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
				   "%s_battery::charging", ec->name);
	led->default_trigger = "default-off";
	led->brightness_set = asusec_led_set_brightness;

	ret = devm_led_classdev_register(&pdev->dev, led);
	if (ret)
		dev_err(&pdev->dev, "can't register LED: %d", ret);

	return ret;
}

static struct platform_driver asusec_led_driver = {
	.driver.name = "asusec-led",
	.probe = asusec_led_probe,
};
module_platform_driver(asusec_led_driver);

MODULE_AUTHOR("Michał Mirosław <mirq-linux@rere.qmqm.pl>");
MODULE_DESCRIPTION("ASUS Transformer's charging LED driver");
MODULE_LICENSE("GPL");
