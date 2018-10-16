// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Avionic Design GmbH
 * Copyright (C) 2013 NVIDIA Corporation
 */

#include <linux/clk.h>
#include <linux/host1x.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/pm_opp.h>

#include <soc/tegra/fuse.h>
#include <soc/tegra/pmc.h>

#include "drm.h"
#include "gem.h"
#include "gr3d.h"

struct gr3d_soc {
	unsigned int version;
};

struct gr3d {
	struct host1x_client client;
	struct clk *clk_secondary;
	struct clk *clk;
	struct reset_control *rst_secondary;
	struct reset_control *rst;

	const struct gr3d_soc *soc;
};

static const struct gr3d_soc tegra20_gr3d_soc = {
	.version = 0x20,
};

static const struct gr3d_soc tegra30_gr3d_soc = {
	.version = 0x30,
};

static const struct gr3d_soc tegra114_gr3d_soc = {
	.version = 0x35,
};

static const struct of_device_id tegra_gr3d_match[] = {
	{ .compatible = "nvidia,tegra114-gr3d", .data = &tegra114_gr3d_soc },
	{ .compatible = "nvidia,tegra30-gr3d", .data = &tegra30_gr3d_soc },
	{ .compatible = "nvidia,tegra20-gr3d", .data = &tegra20_gr3d_soc },
	{ }
};
MODULE_DEVICE_TABLE(of, tegra_gr3d_match);

static int gr3d_init_opp_state(struct device *dev, struct gr3d *gr3d)
{
	struct dev_pm_opp *opp;
	unsigned long rate;
	int err;

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
	rate = clk_get_rate(gr3d->clk);

	/* find suitable OPP for the clock rate supportable by SoC speedo ID */
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
	 * in accordance to the clock rate.  We need to do this because GR2D
	 * currently doesn't support power management and clock is permanently
	 * enabled.
	 */
	err = dev_pm_opp_set_rate(dev, rate);
	if (err) {
		dev_err(dev, "failed to initialize OPP clock: %d\n", err);
		return err;
	}

	return 0;
}

static void gr3d_deinit_opp_table(void *data)
{
	struct device *dev = data;
	struct opp_table *opp_table;

	opp_table = dev_pm_opp_get_opp_table(dev);
	dev_pm_opp_of_remove_table(dev);
	dev_pm_opp_put_supported_hw(opp_table);
	dev_pm_opp_put_regulators(opp_table);
	dev_pm_opp_put_opp_table(opp_table);
}

static int devm_gr3d_init_opp_table(struct device *dev, struct gr3d *gr3d)
{
	struct opp_table *opp_table, *hw_opp_table;
	const char *rname = "core";
	u32 hw_version;
	int err;

	/* voltage scaling is optional */
	if (device_property_present(dev, "core-supply"))
		opp_table = dev_pm_opp_set_regulators(dev, &rname, 1);
	else
		opp_table = dev_pm_opp_get_opp_table(dev);

	if (IS_ERR(opp_table))
		return dev_err_probe(dev, PTR_ERR(opp_table),
				     "failed to prepare OPP table\n");

	if (gr3d->soc->version == 0x20)
		hw_version = BIT(tegra_sku_info.soc_process_id);
	else
		hw_version = BIT(tegra_sku_info.soc_speedo_id);

	hw_opp_table = dev_pm_opp_set_supported_hw(dev, &hw_version, 1);
	err = PTR_ERR_OR_ZERO(hw_opp_table);
	if (err) {
		dev_err(dev, "failed to set supported HW: %d\n", err);
		goto put_table;
	}

	/*
	 * OPP table presence is optional and we want the set_rate() of OPP
	 * API to work similarly to clk_set_rate() if table is missing in a
	 * device-tree.  The add_table() errors out if OPP is missing in DT.
	 */
	if (device_property_present(dev, "operating-points-v2")) {
		err = dev_pm_opp_of_add_table(dev);
		if (err) {
			dev_err(dev, "failed to add OPP table: %d\n", err);
			goto put_hw;
		}

		err = gr3d_init_opp_state(dev, gr3d);
		if (err)
			goto remove_table;
	}

	err = devm_add_action(dev, gr3d_deinit_opp_table, dev);
	if (err)
		goto remove_table;

	dev_info(dev, "OPP HW ver. 0x%x\n", hw_version);

	return 0;

remove_table:
	dev_pm_opp_of_remove_table(dev);
put_hw:
	dev_pm_opp_put_supported_hw(opp_table);
put_table:
	dev_pm_opp_put_regulators(opp_table);

	return err;
}

static int gr3d_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct gr3d *gr3d;
	int err;

	gr3d = devm_kzalloc(&pdev->dev, sizeof(*gr3d), GFP_KERNEL);
	if (!gr3d)
		return -ENOMEM;

	gr3d->soc = of_device_get_match_data(&pdev->dev);

	gr3d->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(gr3d->clk)) {
		dev_err(&pdev->dev, "cannot get clock\n");
		return PTR_ERR(gr3d->clk);
	}

	err = devm_gr3d_init_opp_table(&pdev->dev, gr3d);
	if (err)
		return dev_err_probe(&pdev->dev, err,
				     "failed to initialize OPP\n");

	gr3d->rst = devm_reset_control_get(&pdev->dev, "3d");
	if (IS_ERR(gr3d->rst)) {
		dev_err(&pdev->dev, "cannot get reset\n");
		return PTR_ERR(gr3d->rst);
	}

	if (of_device_is_compatible(np, "nvidia,tegra30-gr3d")) {
		gr3d->clk_secondary = devm_clk_get(&pdev->dev, "3d2");
		if (IS_ERR(gr3d->clk_secondary)) {
			dev_err(&pdev->dev, "cannot get secondary clock\n");
			return PTR_ERR(gr3d->clk_secondary);
		}

		gr3d->rst_secondary = devm_reset_control_get(&pdev->dev,
								"3d2");
		if (IS_ERR(gr3d->rst_secondary)) {
			dev_err(&pdev->dev, "cannot get secondary reset\n");
			return PTR_ERR(gr3d->rst_secondary);
		}
	}

	err = tegra_powergate_sequence_power_up(TEGRA_POWERGATE_3D, gr3d->clk,
						gr3d->rst);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to power up 3D unit\n");
		return err;
	}

	if (gr3d->clk_secondary) {
		err = tegra_powergate_sequence_power_up(TEGRA_POWERGATE_3D1,
							gr3d->clk_secondary,
							gr3d->rst_secondary);
		if (err < 0) {
			dev_err(&pdev->dev,
				"failed to power up secondary 3D unit\n");
			return err;
		}
	}

	INIT_LIST_HEAD(&gr3d->client.list);
	gr3d->client.dev = &pdev->dev;

	err = host1x_client_register(&gr3d->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register host1x client: %d\n",
			err);
		return err;
	}

	platform_set_drvdata(pdev, gr3d);

	return 0;
}

static int gr3d_remove(struct platform_device *pdev)
{
	struct gr3d *gr3d = platform_get_drvdata(pdev);
	int err;

	err = host1x_client_unregister(&gr3d->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to unregister host1x client: %d\n",
			err);
		return err;
	}

	if (gr3d->clk_secondary) {
		reset_control_assert(gr3d->rst_secondary);
		tegra_powergate_power_off(TEGRA_POWERGATE_3D1);
		clk_disable_unprepare(gr3d->clk_secondary);
	}

	reset_control_assert(gr3d->rst);
	tegra_powergate_power_off(TEGRA_POWERGATE_3D);
	clk_disable_unprepare(gr3d->clk);

	return 0;
}

struct platform_driver tegra_gr3d_driver = {
	.driver = {
		.name = "tegra-gr3d",
		.of_match_table = tegra_gr3d_match,
	},
	.probe = gr3d_probe,
	.remove = gr3d_remove,
};
