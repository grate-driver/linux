// SPDX-License-Identifier: GPL-2.0+
/*
 * Power button LED driver for Acer Iconia Tab A500
 */

#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "ec.h"

struct ec_led {
	struct led_classdev cdev;
	const struct ec_reg_data *reg;
};

/*				addr	timeout ms */
EC_REG_DATA(RESET_LEDS,		0x40,	100);
EC_REG_DATA(POWER_LED_ON,	0x42,	100);
EC_REG_DATA(CHARGE_LED_ON,	0x43,	100);
EC_REG_DATA(ANDROID_LEDS_OFF,	0x5A,	100);

static int ec_led_set(struct led_classdev *led_cdev,
		      enum led_brightness value);

static struct ec_led ec_white_led = {
	.cdev = {
		.name			 = "power-button-white",
		.brightness_set_blocking = ec_led_set,
		.max_brightness		 = LED_ON,
		.flags			 = LED_CORE_SUSPENDRESUME,
	},
	.reg = &EC_POWER_LED_ON,
};

static struct ec_led ec_orange_led = {
	.cdev = {
		.name			 = "power-button-orange",
		.brightness_set_blocking = ec_led_set,
		.max_brightness		 = LED_ON,
		.flags			 = LED_CORE_SUSPENDRESUME,
	},
	.reg = &EC_CHARGE_LED_ON,
};

static int ec_led_set(struct led_classdev *led_cdev,
		      enum led_brightness value)
{
	struct ec_led *led = container_of(led_cdev, struct ec_led, cdev);
	int ret;

	a500_ec_lock();

	if (value) {
		ret = a500_ec_write_word_data_locked(led->reg, 0);
	} else {
		ret = a500_ec_write_word_data_locked(RESET_LEDS, 0);
		if (ret)
			goto unlock;

		if (led == &ec_white_led)
			led = &ec_orange_led;
		else
			led = &ec_white_led;

		/* RESET_LEDS turns off both LEDs, thus restore second LED */
		if (led->cdev.brightness == LED_ON)
			ret = a500_ec_write_word_data_locked(led->reg, 0);
	}

unlock:
	a500_ec_unlock();

	return ret;
}

static int ec_leds_probe(struct platform_device *pdev)
{
	int err;

	a500_ec_write_word_data(RESET_LEDS, 0);
	a500_ec_write_word_data(ANDROID_LEDS_OFF, 0);

	err = led_classdev_register(&pdev->dev, &ec_white_led.cdev);
	if (err) {
		dev_err(&pdev->dev, "failed to register white led\n");
		return err;
	}

	err = led_classdev_register(&pdev->dev, &ec_orange_led.cdev);
	if (err) {
		dev_err(&pdev->dev, "failed to register orange led\n");
		goto unreg_white;
	}

	return 0;

unreg_white:
	led_classdev_unregister(&ec_white_led.cdev);

	return err;
}

static int ec_leds_remove(struct platform_device *pdev)
{
	led_classdev_unregister(&ec_white_led.cdev);
	led_classdev_unregister(&ec_orange_led.cdev);

	a500_ec_write_word_data(RESET_LEDS, 0);

	return 0;
}

static const struct of_device_id ec_leds_match[] = {
	{ .compatible = "acer,a500-iconia-leds" },
	{ }
};
MODULE_DEVICE_TABLE(of, ec_leds_match);

static struct platform_driver ec_leds_driver = {
	.driver = {
		.name = "a500-leds",
		.of_match_table = ec_leds_match,
	},
	.probe = ec_leds_probe,
	.remove = ec_leds_remove,
};
module_platform_driver(ec_leds_driver);

MODULE_ALIAS("platform:a500-leds");
MODULE_DESCRIPTION("Acer Iconia Tab A500 Embedded Controller LED driver");
MODULE_AUTHOR("Dmitry Osipenko <digetx@gmail.com>");
MODULE_LICENSE("GPL v2");
