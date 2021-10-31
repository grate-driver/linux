// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon SoC reset code
 *
 * Copyright (c) 2014 HiSilicon Ltd.
 * Copyright (c) 2014 Linaro Ltd.
 *
 * Author: Haojian Zhuang <haojian.zhuang@linaro.org>
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#include <asm/proc-fns.h>

static void hisi_restart(struct restart_data *data)
{
	writel_relaxed(0xdeadbeef, data->cb_data);

	while (1)
		cpu_do_idle();
}

static int hisi_reboot_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	void __iomem *base;
	u32 reboot_offset;
	int err;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (!base) {
		WARN(1, "failed to map base address");
		return -ENODEV;
	}

	if (of_property_read_u32(np, "reboot-offset", &reboot_offset) < 0) {
		pr_err("failed to find reboot-offset property\n");
		return -EINVAL;
	}

	err = devm_register_simple_restart_handler(&pdev->dev, hisi_restart,
						   base + reboot_offset);
	if (err)
		dev_err(&pdev->dev, "cannot register restart handler (err=%d)\n",
			err);

	return err;
}

static const struct of_device_id hisi_reboot_of_match[] = {
	{ .compatible = "hisilicon,sysctrl" },
	{}
};
MODULE_DEVICE_TABLE(of, hisi_reboot_of_match);

static struct platform_driver hisi_reboot_driver = {
	.probe = hisi_reboot_probe,
	.driver = {
		.name = "hisi-reboot",
		.of_match_table = hisi_reboot_of_match,
	},
};
module_platform_driver(hisi_reboot_driver);
