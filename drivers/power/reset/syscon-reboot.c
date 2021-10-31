// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Generic Syscon Reboot Driver
 *
 * Copyright (c) 2013, Applied Micro Circuits Corporation
 * Author: Feng Kan <fkan@apm.com>
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

struct syscon_reboot_context {
	struct regmap *map;
	u32 offset;
	u32 value;
	u32 mask;
};

static void syscon_restart_handle(struct restart_data *data)
{
	struct syscon_reboot_context *ctx = data->cb_data;

	/* Issue the reboot */
	regmap_update_bits(ctx->map, ctx->offset, ctx->mask, ctx->value);

	mdelay(1000);

	pr_emerg("Unable to restart system\n");
}

static int syscon_reboot_probe(struct platform_device *pdev)
{
	struct syscon_reboot_context *ctx;
	struct device *dev = &pdev->dev;
	int mask_err, value_err;
	int err;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->map = syscon_regmap_lookup_by_phandle(dev->of_node, "regmap");
	if (IS_ERR(ctx->map)) {
		ctx->map = syscon_node_to_regmap(dev->parent->of_node);
		if (IS_ERR(ctx->map))
			return PTR_ERR(ctx->map);
	}

	if (of_property_read_u32(pdev->dev.of_node, "offset", &ctx->offset))
		return -EINVAL;

	value_err = of_property_read_u32(pdev->dev.of_node, "value", &ctx->value);
	mask_err = of_property_read_u32(pdev->dev.of_node, "mask", &ctx->mask);
	if (value_err && mask_err) {
		dev_err(dev, "unable to read 'value' and 'mask'");
		return -EINVAL;
	}

	if (value_err) {
		/* support old binding */
		ctx->value = ctx->mask;
		ctx->mask = 0xFFFFFFFF;
	} else if (mask_err) {
		/* support value without mask*/
		ctx->mask = 0xFFFFFFFF;
	}

	err = devm_register_prioritized_restart_handler(dev, RESTART_PRIO_HIGH,
							syscon_restart_handle,
							ctx);
	if (err)
		dev_err(dev, "can't register restart notifier (err=%d)\n", err);

	return err;
}

static const struct of_device_id syscon_reboot_of_match[] = {
	{ .compatible = "syscon-reboot" },
	{}
};

static struct platform_driver syscon_reboot_driver = {
	.probe = syscon_reboot_probe,
	.driver = {
		.name = "syscon-reboot",
		.of_match_table = syscon_reboot_of_match,
	},
};
builtin_platform_driver(syscon_reboot_driver);
