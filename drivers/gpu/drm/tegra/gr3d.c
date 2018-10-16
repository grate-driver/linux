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
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <soc/tegra/common.h>
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
	struct clk_bulk_data clocks[2];
	unsigned int nclocks;
	bool legacy_pd;
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

static void gr3d_pm_runtime_release(void *dev)
{
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
}

static int gr3d_link_power_domain(struct device *dev, struct device *pd_dev)
{
	const u32 link_flags = DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME;
	struct device_link *link;
	int err;

	link = device_link_add(dev, pd_dev, link_flags);
	if (!link) {
		dev_err(dev, "failed to link to %s\n", dev_name(pd_dev));
		return -EINVAL;
	}

	err = devm_add_action_or_reset(dev, (void*)device_link_del, link);
	if (err)
		return err;

	return 0;
}

static int devm_gr3d_init_power(struct device *dev, struct gr3d *gr3d)
{
	const char *opp_genpd_names[] = { "3d0", "3d1", NULL };
	struct device **opp_virt_dev;
	struct opp_table *opp_table;
	unsigned int i, num_domains;
	struct device *pd_dev;
	int err;

	err = of_count_phandle_with_args(dev->of_node, "power-domains",
					 "#power-domain-cells");
	if (err < 0) {
		if (err != -ENOENT)
			return err;

		/*
		 * Older device-trees don't use GENPD. In this case we should
		 * toggle power domain manually.
		 */
		gr3d->legacy_pd = true;
		goto power_up;
	}

	num_domains = err;

	/*
	 * The PM domain core automatically attaches a single power domain,
	 * otherwise it skips attaching completely. We have a single domain
	 * on Tegra20 and two domains on Tegra30+.
	 */
	if (dev->pm_domain)
		goto power_up;

	opp_table = devm_pm_opp_attach_genpd(dev, opp_genpd_names, &opp_virt_dev);
	if (IS_ERR(opp_table))
		return PTR_ERR(opp_table);

	for (i = 0; opp_genpd_names[i]; i++) {
		pd_dev = opp_virt_dev[i];
		if (!pd_dev) {
			dev_err(dev, "failed to get %s power domain\n",
				opp_genpd_names[i]);
			return -EINVAL;
		}

		err = gr3d_link_power_domain(dev, pd_dev);
		if (err)
			return err;
	}

power_up:
	pm_runtime_enable(dev);
	err = pm_runtime_get_sync(dev);
	if (err < 0) {
		gr3d_pm_runtime_release(dev);
		return err;
	}

	err = devm_add_action_or_reset(dev, gr3d_pm_runtime_release, dev);
	if (err)
		return err;

	return 0;
}

static int gr3d_set_opp(struct dev_pm_set_opp_data *data)
{
	struct gr3d *gr3d = dev_get_drvdata(data->dev);
	unsigned int i;
	int err;

	for (i = 0; i < gr3d->nclocks; i++) {
		err = clk_set_rate(gr3d->clocks[i].clk, data->new_opp.rate);
		if (err) {
			dev_err(data->dev, "failed to set %s rate to %lu: %d\n",
				gr3d->clocks[i].id, data->new_opp.rate, err);
			return err;
		}
	}

	return 0;
}

static int gr3d_probe(struct platform_device *pdev)
{
	struct tegra_core_opp_params opp_params = {};
	struct device_node *np = pdev->dev.of_node;
	struct opp_table *opp_table;
	struct gr3d *gr3d;
	int err;

	gr3d = devm_kzalloc(&pdev->dev, sizeof(*gr3d), GFP_KERNEL);
	if (!gr3d)
		return -ENOMEM;

	platform_set_drvdata(pdev, gr3d);

	gr3d->soc = of_device_get_match_data(&pdev->dev);

	gr3d->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(gr3d->clk)) {
		dev_err(&pdev->dev, "cannot get clock\n");
		return PTR_ERR(gr3d->clk);
	}

	gr3d->clocks[gr3d->nclocks].id = "3d";
	gr3d->clocks[gr3d->nclocks].clk = gr3d->clk;
	gr3d->nclocks++;

	gr3d->rst = devm_reset_control_get_exclusive_released(&pdev->dev, "3d");
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

		gr3d->clocks[gr3d->nclocks].id = "3d2";
		gr3d->clocks[gr3d->nclocks].clk = gr3d->clk_secondary;
		gr3d->nclocks++;

		gr3d->rst_secondary =
			devm_reset_control_get_exclusive_released(&pdev->dev, "3d2");
		if (IS_ERR(gr3d->rst_secondary)) {
			dev_err(&pdev->dev, "cannot get secondary reset\n");
			return PTR_ERR(gr3d->rst_secondary);
		}
	}

	err = devm_gr3d_init_power(&pdev->dev, gr3d);
	if (err)
		return err;

	opp_table = devm_pm_opp_register_set_opp_helper(&pdev->dev, gr3d_set_opp);
	if (IS_ERR(opp_table))
		return PTR_ERR(opp_table);

	opp_params.init_state = true;

	err = devm_tegra_core_dev_init_opp_table(&pdev->dev, &opp_params);
	if (err && err != -ENODEV)
		return err;

	INIT_LIST_HEAD(&gr3d->client.list);
	gr3d->client.dev = &pdev->dev;

	err = host1x_client_register(&gr3d->client);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register host1x client: %d\n",
			err);
		return err;
	}

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

	return 0;
}

static int __maybe_unused gr3d_runtime_suspend(struct device *dev)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	int err;

	if (gr3d->legacy_pd && gr3d->clk_secondary) {
		err = reset_control_assert(gr3d->rst_secondary);
		if (err) {
			dev_err(dev, "failed to assert secondary reset: %d\n", err);
			return err;
		}

		tegra_powergate_power_off(TEGRA_POWERGATE_3D1);
	}

	if (gr3d->legacy_pd) {
		err = reset_control_assert(gr3d->rst);
		if (err) {
			dev_err(dev, "failed to assert reset: %d\n", err);
			return err;
		}

		tegra_powergate_power_off(TEGRA_POWERGATE_3D);
	}

	clk_bulk_disable_unprepare(gr3d->nclocks, gr3d->clocks);
	reset_control_release(gr3d->rst_secondary);
	reset_control_release(gr3d->rst);

	return 0;
}

static int __maybe_unused gr3d_runtime_resume(struct device *dev)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	int err;

	err = reset_control_acquire(gr3d->rst);
	if (err) {
		dev_err(dev, "failed to acquire reset: %d\n", err);
		return err;
	}

	err = reset_control_acquire(gr3d->rst_secondary);
	if (err) {
		dev_err(dev, "failed to acquire secondary reset: %d\n", err);
		goto release_reset_primary;
	}

	if (gr3d->legacy_pd) {
		err = tegra_powergate_sequence_power_up(TEGRA_POWERGATE_3D,
							gr3d->clk, gr3d->rst);
		if (err)
			goto release_reset_secondary;
	}

	if (gr3d->legacy_pd && gr3d->clk_secondary) {
		err = tegra_powergate_sequence_power_up(TEGRA_POWERGATE_3D1,
							gr3d->clk_secondary,
							gr3d->rst_secondary);
		if (err)
			goto release_reset_secondary;
	}

	err = clk_bulk_prepare_enable(gr3d->nclocks, gr3d->clocks);
	if (err) {
		dev_err(dev, "failed to enable clock: %d\n", err);
		goto release_reset_secondary;
	}

	return 0;

release_reset_secondary:
	reset_control_release(gr3d->rst_secondary);

release_reset_primary:
	reset_control_release(gr3d->rst);

	return err;
}

static __maybe_unused int gr3d_suspend(struct device *dev)
{
	int err;

	err = pm_runtime_force_suspend(dev);
	if (err < 0)
		return err;

	return 0;
}

static const struct dev_pm_ops tegra_gr3d_pm = {
	SET_RUNTIME_PM_OPS(gr3d_runtime_suspend, gr3d_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(gr3d_suspend, pm_runtime_force_resume)
};

struct platform_driver tegra_gr3d_driver = {
	.driver = {
		.name = "tegra-gr3d",
		.of_match_table = tegra_gr3d_match,
		.pm = &tegra_gr3d_pm,
	},
	.probe = gr3d_probe,
	.remove = gr3d_remove,
};
