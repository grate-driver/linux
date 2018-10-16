// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2013, NVIDIA Corporation.
 */

#include <linux/clk.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of_device.h>

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
