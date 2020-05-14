// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2014 NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/sysfs.h>

#include <soc/tegra/common.h>

struct kobject *tegra_soc_kobj;

static const struct of_device_id tegra_machine_match[] = {
	{ .compatible = "nvidia,tegra20", },
	{ .compatible = "nvidia,tegra30", },
	{ .compatible = "nvidia,tegra114", },
	{ .compatible = "nvidia,tegra124", },
	{ .compatible = "nvidia,tegra132", },
	{ .compatible = "nvidia,tegra210", },
	{ }
};

bool soc_is_tegra(void)
{
	const struct of_device_id *match;
	struct device_node *root;

	root = of_find_node_by_path("/");
	if (!root)
		return false;

	match = of_match_node(tegra_machine_match, root);
	of_node_put(root);

	return match != NULL;
}

static int __init tegra_soc_sysfs_init(void)
{
	if (!soc_is_tegra())
		return 0;

	tegra_soc_kobj = kobject_create_and_add("tegra", NULL);
	WARN_ON(!tegra_soc_kobj);

	return 0;
}
arch_initcall(tegra_soc_sysfs_init)
