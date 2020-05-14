// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2014 NVIDIA CORPORATION.  All rights reserved.
 */

#define dev_fmt(fmt)	"%s: " fmt, __func__
#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/export.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/sysfs.h>

#include <soc/tegra/common.h>

#define terga_soc_for_each_device(__dev) \
	for ((__dev) = tegra_soc_devices; (__dev) && (__dev)->compatible; \
	     (__dev)++)

struct tegra_soc_device {
	const char *compatible;
	const bool dvfs_critical;
	unsigned int sync_count;
};

static DEFINE_MUTEX(tegra_soc_lock);
static struct tegra_soc_device *tegra_soc_devices;
struct kobject *tegra_soc_kobj;

/*
 * DVFS-critical devices are either active at a boot time or permanently
 * active, like EMC for example.  System-wide DVFS should be deferred until
 * drivers of the critical devices synced theirs state.
 */

static struct tegra_soc_device tegra20_soc_devices[] = {
	{ .compatible = "nvidia,tegra20-dc", .dvfs_critical = true, },
	{ .compatible = "nvidia,tegra20-emc", .dvfs_critical = true, },
	{ }
};

static struct tegra_soc_device tegra30_soc_devices[] = {
	{ .compatible = "nvidia,tegra30-dc", .dvfs_critical = true, },
	{ .compatible = "nvidia,tegra30-emc", .dvfs_critical = true, },
	{ .compatible = "nvidia,tegra30-pwm", .dvfs_critical = true, },
	{ }
};

static const struct of_device_id tegra_machine_match[] = {
	{ .compatible = "nvidia,tegra20", .data = tegra20_soc_devices, },
	{ .compatible = "nvidia,tegra30", .data = tegra30_soc_devices, },
	{ .compatible = "nvidia,tegra114", },
	{ .compatible = "nvidia,tegra124", },
	{ .compatible = "nvidia,tegra132", },
	{ .compatible = "nvidia,tegra210", },
	{ }
};

static const struct of_device_id *tegra_soc_of_match(void)
{
	const struct of_device_id *match;
	struct device_node *root;

	root = of_find_node_by_path("/");
	if (!root)
		return false;

	match = of_match_node(tegra_machine_match, root);
	of_node_put(root);

	return match;
}

bool soc_is_tegra(void)
{
	return tegra_soc_of_match() != NULL;
}

void tegra_soc_device_sync_state(struct device *dev)
{
	struct tegra_soc_device *soc_dev;

	mutex_lock(&tegra_soc_lock);
	terga_soc_for_each_device(soc_dev) {
		if (!of_device_is_compatible(dev->of_node, soc_dev->compatible))
			continue;

		if (!soc_dev->sync_count) {
			dev_err(dev, "already synced\n");
			break;
		}

		/*
		 * All DVFS-capable devices should have the CORE regulator
		 * phandle.  Older device-trees don't have it, hence state
		 * won't be synced for the older DTBs, allowing them to work
		 * properly.
		 */
		if (soc_dev->dvfs_critical &&
		    !device_property_present(dev, "core-supply")) {
			dev_dbg(dev, "doesn't have core supply\n");
			break;
		}

		soc_dev->sync_count--;
		dev_dbg(dev, "sync_count=%u\n", soc_dev->sync_count);
		break;
	}
	mutex_unlock(&tegra_soc_lock);
}
EXPORT_SYMBOL_GPL(tegra_soc_device_sync_state);

bool tegra_soc_dvfs_state_synced(void)
{
	struct tegra_soc_device *soc_dev;
	bool synced_state = true;

	/*
	 * CORE voltage scaling is limited until drivers of the critical
	 * devices synced theirs state.
	 */
	mutex_lock(&tegra_soc_lock);
	terga_soc_for_each_device(soc_dev) {
		if (!soc_dev->sync_count || !soc_dev->dvfs_critical)
			continue;

		pr_debug_ratelimited("%s: sync_count=%u\n",
				     soc_dev->compatible, soc_dev->sync_count);

		synced_state = false;
		break;
	}
	mutex_unlock(&tegra_soc_lock);

	return synced_state;
}

static int __init tegra_soc_devices_init(void)
{
	struct device_node *np, *prev_np = NULL;
	struct tegra_soc_device *soc_dev;
	const struct of_device_id *match;

	if (!soc_is_tegra())
		return 0;

	match = tegra_soc_of_match();
	tegra_soc_devices = (void *)match->data;

	/*
	 * If device node is disabled in a device-tree, then we shouldn't
	 * care about this device. Even if device is active during boot,
	 * its clock will be disabled by CCF as unused.
	 */
	terga_soc_for_each_device(soc_dev) {
		do {
			/*
			 * Devices like display controller have multiple
			 * instances with the same compatible. Hence we need
			 * to walk up the whole tree in order to account those
			 * multiple instances.
			 */
			np = of_find_compatible_node(prev_np, NULL,
						     soc_dev->compatible);
			of_node_put(prev_np);
			prev_np = np;

			if (of_device_is_available(np)) {
				pr_debug("added %s\n", soc_dev->compatible);
				soc_dev->sync_count++;
			}
		} while (np);
	}

	return 0;
}
postcore_initcall_sync(tegra_soc_devices_init);

static int __init tegra_soc_sysfs_init(void)
{
	if (!soc_is_tegra())
		return 0;

	tegra_soc_kobj = kobject_create_and_add("tegra", NULL);
	WARN_ON(!tegra_soc_kobj);

	return 0;
}
arch_initcall(tegra_soc_sysfs_init);
