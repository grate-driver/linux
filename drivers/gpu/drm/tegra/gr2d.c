// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2013, NVIDIA Corporation.
 */

#include <linux/clk.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_opp.h>

#include <soc/tegra/fuse.h>

#include "drm.h"
#include "gem.h"
#include "gr2d.h"

struct gr2d_soc {
	unsigned int version;
};

struct gr2d {
	struct host1x_client client;
	struct clk *clk;

	const struct gr2d_soc *soc;
};

static const struct gr2d_soc tegra20_gr2d_soc = {
	.version = 0x20,
};

static const struct gr2d_soc tegra30_gr2d_soc = {
	.version = 0x30,
};

static const struct of_device_id gr2d_match[] = {
	{ .compatible = "nvidia,tegra30-gr2d", .data = &tegra30_gr2d_soc },
	{ .compatible = "nvidia,tegra20-gr2d", .data = &tegra20_gr2d_soc },
	{ },
};
MODULE_DEVICE_TABLE(of, gr2d_match);

static int gr2d_init_opp_state(struct device *dev, struct gr2d *gr2d)
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
	rate = clk_get_rate(gr2d->clk);

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

static void gr2d_deinit_opp_table(void *data)
{
	struct device *dev = data;
	struct opp_table *opp_table;

	opp_table = dev_pm_opp_get_opp_table(dev);
	dev_pm_opp_of_remove_table(dev);
	dev_pm_opp_put_supported_hw(opp_table);
	dev_pm_opp_put_regulators(opp_table);
	dev_pm_opp_put_opp_table(opp_table);
}

static int devm_gr2d_init_opp_table(struct device *dev, struct gr2d *gr2d)
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

	if (gr2d->soc->version == 0x20)
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

		err = gr2d_init_opp_state(dev, gr2d);
		if (err)
			goto remove_table;
	}

	err = devm_add_action(dev, gr2d_deinit_opp_table, dev);
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

static int gr2d_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gr2d *gr2d;
	int err;

	gr2d = devm_kzalloc(dev, sizeof(*gr2d), GFP_KERNEL);
	if (!gr2d)
		return -ENOMEM;

	gr2d->soc = of_device_get_match_data(dev);

	gr2d->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(gr2d->clk)) {
		dev_err(dev, "cannot get clock\n");
		return PTR_ERR(gr2d->clk);
	}

	err = devm_gr2d_init_opp_table(dev, gr2d);
	if (err)
		return dev_err_probe(dev, err, "failed to initialize OPP\n");

	err = clk_prepare_enable(gr2d->clk);
	if (err) {
		dev_err(dev, "cannot turn on clock\n");
		return err;
	}

	INIT_LIST_HEAD(&gr2d->client.list);
	gr2d->client.dev = dev;

	err = host1x_client_register(&gr2d->client);
	if (err < 0) {
		dev_err(dev, "failed to register host1x client: %d\n", err);
		clk_disable_unprepare(gr2d->clk);
		return err;
	}

	platform_set_drvdata(pdev, gr2d);

	return 0;
}

static int gr2d_remove(struct platform_device *pdev)
{
	struct gr2d *gr2d = platform_get_drvdata(pdev);
	int err;

	err = host1x_client_unregister(&gr2d->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to unregister host1x client: %d\n",
			err);
		return err;
	}

	clk_disable_unprepare(gr2d->clk);

	return 0;
}

struct platform_driver tegra_gr2d_driver = {
	.driver = {
		.name = "tegra-gr2d",
		.of_match_table = gr2d_match,
	},
	.probe = gr2d_probe,
	.remove = gr2d_remove,
};
