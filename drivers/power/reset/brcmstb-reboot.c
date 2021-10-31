/*
 * Copyright (C) 2013 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/notifier.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/smp.h>
#include <linux/mfd/syscon.h>

#define RESET_SOURCE_ENABLE_REG 1
#define SW_MASTER_RESET_REG 2

struct reset_reg_mask {
	u32 rst_src_en_mask;
	u32 sw_mstr_rst_mask;
};

struct brcmstb_data {
	struct regmap *regmap;
	u32 rst_src_en;
	u32 sw_mstr_rst;
	const struct reset_reg_mask *reset_masks;
};

static void brcmstb_restart_handler(struct restart_data *data)
{
	struct brcmstb_data *bd = data->cb_data;
	const struct reset_reg_mask *reset_masks = bd->reset_masks;
	struct regmap *regmap = bd->regmap;
	u32 sw_mstr_rst = bd->sw_mstr_rst;
	u32 rst_src_en = bd->rst_src_en;
	int rc;
	u32 tmp;

	rc = regmap_write(regmap, rst_src_en, reset_masks->rst_src_en_mask);
	if (rc) {
		pr_err("failed to write rst_src_en (%d)\n", rc);
		return;
	}

	rc = regmap_read(regmap, rst_src_en, &tmp);
	if (rc) {
		pr_err("failed to read rst_src_en (%d)\n", rc);
		return;
	}

	rc = regmap_write(regmap, sw_mstr_rst, reset_masks->sw_mstr_rst_mask);
	if (rc) {
		pr_err("failed to write sw_mstr_rst (%d)\n", rc);
		return;
	}

	rc = regmap_read(regmap, sw_mstr_rst, &tmp);
	if (rc) {
		pr_err("failed to read sw_mstr_rst (%d)\n", rc);
		return;
	}

	while (1)
		;
}

static const struct reset_reg_mask reset_bits_40nm = {
	.rst_src_en_mask = BIT(0),
	.sw_mstr_rst_mask = BIT(0),
};

static const struct reset_reg_mask reset_bits_65nm = {
	.rst_src_en_mask = BIT(3),
	.sw_mstr_rst_mask = BIT(31),
};

static const struct of_device_id of_match[] = {
	{ .compatible = "brcm,brcmstb-reboot", .data = &reset_bits_40nm },
	{ .compatible = "brcm,bcm7038-reboot", .data = &reset_bits_65nm },
	{},
};

static int brcmstb_reboot_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	const struct reset_reg_mask *reset_masks;
	const struct of_device_id *of_id;
	struct device *dev = &pdev->dev;
	u32 rst_src_en, sw_mstr_rst;
	struct brcmstb_data *bd;
	struct regmap *regmap;
	int rc;

	of_id = of_match_node(of_match, np);
	if (!of_id) {
		pr_err("failed to look up compatible string\n");
		return -EINVAL;
	}
	reset_masks = of_id->data;

	regmap = syscon_regmap_lookup_by_phandle(np, "syscon");
	if (IS_ERR(regmap)) {
		pr_err("failed to get syscon phandle\n");
		return -EINVAL;
	}

	rc = of_property_read_u32_index(np, "syscon", RESET_SOURCE_ENABLE_REG,
					&rst_src_en);
	if (rc) {
		pr_err("can't get rst_src_en offset (%d)\n", rc);
		return -EINVAL;
	}

	rc = of_property_read_u32_index(np, "syscon", SW_MASTER_RESET_REG,
					&sw_mstr_rst);
	if (rc) {
		pr_err("can't get sw_mstr_rst offset (%d)\n", rc);
		return -EINVAL;
	}

	bd = devm_kzalloc(dev, sizeof(*bd), GFP_KERNEL);
	if (!bd)
		return -ENOMEM;

	bd->regmap = regmap;
	bd->rst_src_en = rst_src_en;
	bd->sw_mstr_rst = sw_mstr_rst;

	rc = devm_register_simple_restart_handler(dev, brcmstb_restart_handler,
						  bd);
	if (rc)
		dev_err(&pdev->dev,
			"cannot register restart handler (err=%d)\n", rc);

	return rc;
}

static struct platform_driver brcmstb_reboot_driver = {
	.probe = brcmstb_reboot_probe,
	.driver = {
		.name = "brcmstb-reboot",
		.of_match_table = of_match,
	},
};

static int __init brcmstb_reboot_init(void)
{
	return platform_driver_probe(&brcmstb_reboot_driver,
					brcmstb_reboot_probe);
}
subsys_initcall(brcmstb_reboot_init);
