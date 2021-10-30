// SPDX-License-Identifier: GPL-2.0-only
/*
 * Force-disables a regulator to power down a device
 *
 * Michael Klein <michael@fossekall.de>
 *
 * Copyright (C) 2020 Michael Klein
 *
 * Based on the gpio-poweroff driver.
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regulator/consumer.h>

#define TIMEOUT_MS 3000

static void regulator_poweroff_do_poweroff(void *data)
{
	struct regulator *cpu_regulator = data;

	if (cpu_regulator && regulator_is_enabled(cpu_regulator))
		regulator_force_disable(cpu_regulator);

	/* give it some time */
	mdelay(TIMEOUT_MS);

	WARN_ON(1);
}

static int regulator_poweroff_probe(struct platform_device *pdev)
{
	struct regulator *cpu_regulator;

	cpu_regulator = devm_regulator_get(&pdev->dev, "cpu");
	if (IS_ERR(cpu_regulator))
		return PTR_ERR(cpu_regulator);

	return devm_register_simple_power_off_handler(&pdev->dev,
						      regulator_poweroff_do_poweroff,
						      cpu_regulator);
}

static const struct of_device_id of_regulator_poweroff_match[] = {
	{ .compatible = "regulator-poweroff", },
	{},
};
MODULE_DEVICE_TABLE(of, of_regulator_poweroff_match);

static struct platform_driver regulator_poweroff_driver = {
	.probe = regulator_poweroff_probe,
	.driver = {
		.name = "poweroff-regulator",
		.of_match_table = of_regulator_poweroff_match,
	},
};

module_platform_driver(regulator_poweroff_driver);

MODULE_AUTHOR("Michael Klein <michael@fossekall.de>");
MODULE_DESCRIPTION("Regulator poweroff driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:poweroff-regulator");
