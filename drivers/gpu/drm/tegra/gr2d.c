// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2013, NVIDIA Corporation.
 */

#include <linux/clk.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>

#include <soc/tegra/common.h>

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

static void gr2d_pm_runtime_release(void *dev)
{
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
}

static int gr2d_probe(struct platform_device *pdev)
{
	struct tegra_core_opp_params opp_params = {};
	struct device *dev = &pdev->dev;
	struct gr2d *gr2d;
	int err;

	gr2d = devm_kzalloc(dev, sizeof(*gr2d), GFP_KERNEL);
	if (!gr2d)
		return -ENOMEM;

	platform_set_drvdata(pdev, gr2d);

	gr2d->soc = of_device_get_match_data(dev);

	gr2d->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(gr2d->clk)) {
		dev_err(dev, "cannot get clock\n");
		return PTR_ERR(gr2d->clk);
	}

	opp_params.init_state = true;

	err = devm_tegra_core_dev_init_opp_table(dev, &opp_params);
	if (err && err != -ENODEV)
		return err;

	pm_runtime_enable(dev);
	err = pm_runtime_get_sync(dev);
	if (err < 0) {
		gr2d_pm_runtime_release(dev);
		return err;
	}

	err = devm_add_action_or_reset(dev, gr2d_pm_runtime_release, dev);
	if (err)
		return err;

	INIT_LIST_HEAD(&gr2d->client.list);
	gr2d->client.dev = dev;

	err = host1x_client_register(&gr2d->client);
	if (err < 0) {
		dev_err(dev, "failed to register host1x client: %d\n", err);
		return err;
	}

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

	return 0;
}

static int __maybe_unused gr2d_runtime_suspend(struct device *dev)
{
	struct gr2d *gr2d = dev_get_drvdata(dev);

	clk_disable_unprepare(gr2d->clk);

	return 0;
}

static int __maybe_unused gr2d_runtime_resume(struct device *dev)
{
	struct gr2d *gr2d = dev_get_drvdata(dev);
	int err;

	err = clk_prepare_enable(gr2d->clk);
	if (err) {
		dev_err(dev, "failed to enable clock: %d\n", err);
		return err;
	}

	return 0;
}

static __maybe_unused int gr2d_suspend(struct device *dev)
{
	int err;

	err = pm_runtime_force_suspend(dev);
	if (err < 0)
		return err;

	return 0;
}

static const struct dev_pm_ops tegra_gr2d_pm = {
	SET_RUNTIME_PM_OPS(gr2d_runtime_suspend, gr2d_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(gr2d_suspend, pm_runtime_force_resume)
};

struct platform_driver tegra_gr2d_driver = {
	.driver = {
		.name = "tegra-gr2d",
		.of_match_table = gr2d_match,
		.pm = &tegra_gr2d_pm,
	},
	.probe = gr2d_probe,
	.remove = gr2d_remove,
};
