// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2014 NVIDIA CORPORATION.  All rights reserved.
 */

#define dev_fmt(fmt)	"tegra-soc: " fmt

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/of.h>
#include <linux/pm_opp.h>

#include <soc/tegra/common.h>
#include <soc/tegra/fuse.h>

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

static int tegra_core_dev_init_opp_state(struct device *dev)
{
	struct dev_pm_opp *opp;
	unsigned long rate;
	struct clk *clk;
	int err;

	clk = devm_clk_get(dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(dev, "failed to get clk: %pe\n", clk);
		return PTR_ERR(clk);
	}

	/*
	 * If voltage regulator presents, then we could select the fastest
	 * clock rate, but driver doesn't support power management and
	 * frequency scaling yet, hence the top freq OPP will vote for a
	 * very high voltage that will produce lot's of heat.  Let's select
	 * OPP for the current/default rate for now.
	 *
	 * Clock rate should be pre-initialized (i.e. it's non-zero) either
	 * by clock driver or by assigned clocks in a device-tree.
	 */
	rate = clk_get_rate(clk);
	if (!rate) {
		dev_err(dev, "failed to get clk rate\n");
		return -EINVAL;
	}

	/* find suitable OPP for the clock rate and supportable by hardware */
	opp = dev_pm_opp_find_freq_ceil(dev, &rate);

	/*
	 * dev_pm_opp_set_rate() doesn't search for a floor clock rate and it
	 * will error out if default clock rate is too high, i.e. unsupported
	 * by a SoC hardware version.  Hence will find floor rate by ourselves.
	 */
	if (opp == ERR_PTR(-ERANGE))
		opp = dev_pm_opp_find_freq_floor(dev, &rate);

	err = PTR_ERR_OR_ZERO(opp);
	if (err) {
		dev_err(dev, "failed to get OPP for %ld Hz: %d\n",
			rate, err);
		return err;
	}

	dev_pm_opp_put(opp);

	/*
	 * First dummy rate-set initializes voltage vote by setting voltage
	 * in accordance to the clock rate.  We need to do this because some
	 * drivers currently don't support power management and clock is
	 * permanently enabled.
	 */
	err = dev_pm_opp_set_rate(dev, rate);
	if (err) {
		dev_err(dev, "failed to initialize OPP clock: %d\n", err);
		return err;
	}

	return 0;
}

/**
 * devm_tegra_core_dev_init_opp_table() - initialize OPP table
 * @dev: device for which OPP table is initialized
 * @params: pointer to the OPP table configuration
 *
 * This function will initialize OPP table and sync OPP state of a Tegra SoC
 * core device.
 *
 * Return: 0 on success or errorno.
 */
int devm_tegra_core_dev_init_opp_table(struct device *dev,
				       struct tegra_core_opp_params *params)
{
	u32 hw_version;
	int err;

	err = devm_pm_opp_set_clkname(dev, NULL);
	if (err) {
		dev_err(dev, "failed to set OPP clk: %d\n", err);
		return err;
	}

	/* Tegra114+ doesn't support OPP yet */
	if (!of_machine_is_compatible("nvidia,tegra20") &&
	    !of_machine_is_compatible("nvidia,tegra30"))
		return -ENODEV;

	if (of_machine_is_compatible("nvidia,tegra20"))
		hw_version = BIT(tegra_sku_info.soc_process_id);
	else
		hw_version = BIT(tegra_sku_info.soc_speedo_id);

	err = devm_pm_opp_set_supported_hw(dev, &hw_version, 1);
	if (err) {
		dev_err(dev, "failed to set OPP supported HW: %d\n", err);
		return err;
	}

	/*
	 * Older device-trees have an empty OPP table, hence we will get
	 * -ENODEV from devm_pm_opp_of_add_table() for the older DTBs.
	 *
	 * The OPP table presence also varies per-device and depending
	 * on a SoC generation, hence -ENODEV is expected to happen for
	 * the newer DTs as well.
	 */
	err = devm_pm_opp_of_add_table(dev);
	if (err) {
		if (err == -ENODEV)
			dev_err_once(dev, "OPP table not found, please update device-tree\n");
		else
			dev_err(dev, "failed to add OPP table: %d\n", err);

		return err;
	}

	if (params->init_state) {
		err = tegra_core_dev_init_opp_state(dev);
		if (err)
			return err;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(devm_tegra_core_dev_init_opp_table);
