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
