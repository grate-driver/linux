// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2012 ARM Limited
 */

#include <linux/delay.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/stat.h>
#include <linux/vexpress.h>

static void vexpress_reset_do(struct device *dev, const char *what)
{
	int err = -ENOENT;
	struct regmap *reg = dev_get_drvdata(dev);

	if (reg) {
		err = regmap_write(reg, 0, 0);
		if (!err)
			mdelay(1000);
	}

	dev_emerg(dev, "Unable to %s (%d)\n", what, err);
}

static void vexpress_power_off(void *dev)
{
	vexpress_reset_do(dev, "power off");
}

static struct device *vexpress_restart_device;

static void vexpress_restart(struct restart_data *data)
{
	if (vexpress_restart_device == data->cb_data)
		vexpress_reset_do(data->cb_data, "restart");
}

static ssize_t vexpress_reset_active_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", vexpress_restart_device == dev);
}

static ssize_t vexpress_reset_active_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	long value;
	int err = kstrtol(buf, 0, &value);

	if (!err && value)
		vexpress_restart_device = dev;

	return err ? err : count;
}

static DEVICE_ATTR(active, S_IRUGO | S_IWUSR, vexpress_reset_active_show,
		   vexpress_reset_active_store);


enum vexpress_reset_func { FUNC_RESET, FUNC_SHUTDOWN, FUNC_REBOOT };

static const struct of_device_id vexpress_reset_of_match[] = {
	{
		.compatible = "arm,vexpress-reset",
		.data = (void *)FUNC_RESET,
	}, {
		.compatible = "arm,vexpress-shutdown",
		.data = (void *)FUNC_SHUTDOWN
	}, {
		.compatible = "arm,vexpress-reboot",
		.data = (void *)FUNC_REBOOT
	},
	{}
};

static int vexpress_reset_probe(struct platform_device *pdev)
{
	const struct of_device_id *match =
			of_match_device(vexpress_reset_of_match, &pdev->dev);
	struct regmap *regmap;
	int ret = 0;

	if (!match)
		return -EINVAL;

	regmap = devm_regmap_init_vexpress_config(&pdev->dev);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);
	dev_set_drvdata(&pdev->dev, regmap);

	switch ((enum vexpress_reset_func)match->data) {
	case FUNC_SHUTDOWN:
		ret = devm_register_simple_power_off_handler(&pdev->dev,
							     vexpress_power_off,
							     &pdev->dev);
		break;
	case FUNC_RESET:
		ret = devm_register_prioritized_restart_handler(&pdev->dev,
								RESTART_PRIO_DEFAULT + 0,
								vexpress_restart,
								&pdev->dev);
		if (!ret && !vexpress_restart_device) {
			device_create_file(&pdev->dev, &dev_attr_active);
			vexpress_restart_device = &pdev->dev;
		}
		break;
	case FUNC_REBOOT:
		ret = devm_register_prioritized_restart_handler(&pdev->dev,
								RESTART_PRIO_DEFAULT + 1,
								vexpress_restart,
								&pdev->dev);
		if (!ret) {
			device_create_file(&pdev->dev, &dev_attr_active);
			vexpress_restart_device = &pdev->dev;
		}
		break;
	}

	return ret;
}

static struct platform_driver vexpress_reset_driver = {
	.probe = vexpress_reset_probe,
	.driver = {
		.name = "vexpress-reset",
		.of_match_table = vexpress_reset_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(vexpress_reset_driver);
