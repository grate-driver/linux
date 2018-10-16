// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tegra host1x driver
 *
 * Copyright (c) 2010-2013, NVIDIA Corporation.
 */

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/slab.h>

#include "bus.h"
#include "dev.h"

static const struct host1x_info host1x01_info = {
	.nb_channels = 8,
	.nb_pts = 32,
	.nb_mlocks = 16,
	.nb_bases = 8,
	.sync_offset = 0x3000,
	.dma_mask = DMA_BIT_MASK(32),
};

static const struct host1x_info host1x02_info = {
	.nb_channels = 9,
	.nb_pts = 32,
	.nb_mlocks = 16,
	.nb_bases = 12,
	.sync_offset = 0x3000,
	.dma_mask = DMA_BIT_MASK(32),
};

static const struct host1x_info host1x04_info = {
	.nb_channels = 12,
	.nb_pts = 192,
	.nb_mlocks = 16,
	.nb_bases = 64,
	.sync_offset = 0x2100,
	.dma_mask = DMA_BIT_MASK(34),
};

static const struct host1x_info host1x05_info = {
	.nb_channels = 14,
	.nb_pts = 192,
	.nb_mlocks = 16,
	.nb_bases = 64,
	.sync_offset = 0x2100,
	.dma_mask = DMA_BIT_MASK(34),
};

static const struct host1x_sid_entry tegra186_sid_table[] = {
	{
		/* VIC */
		.base = 0x1af0,
		.offset = 0x30,
		.limit = 0x34
	},
};

static const struct host1x_info host1x06_info = {
	.nb_channels = 63,
	.nb_pts = 576,
	.nb_mlocks = 24,
	.nb_bases = 16,
	.sync_offset = 0x0,
	.dma_mask = DMA_BIT_MASK(40),
	.has_hypervisor = true,
	.num_sid_entries = ARRAY_SIZE(tegra186_sid_table),
	.sid_table = tegra186_sid_table,
};

static const struct host1x_sid_entry tegra194_sid_table[] = {
	{
		/* VIC */
		.base = 0x1af0,
		.offset = 0x30,
		.limit = 0x34
	},
};

static const struct host1x_info host1x07_info = {
	.nb_channels = 63,
	.nb_pts = 704,
	.nb_mlocks = 32,
	.nb_bases = 0,
	.sync_offset = 0x0,
	.dma_mask = DMA_BIT_MASK(40),
	.has_hypervisor = true,
	.num_sid_entries = ARRAY_SIZE(tegra194_sid_table),
	.sid_table = tegra194_sid_table,
};

static const struct of_device_id host1x_of_match[] = {
	{ .compatible = "nvidia,tegra194-host1x", .data = &host1x07_info, },
	{ .compatible = "nvidia,tegra186-host1x", .data = &host1x06_info, },
	{ .compatible = "nvidia,tegra210-host1x", .data = &host1x05_info, },
	{ .compatible = "nvidia,tegra124-host1x", .data = &host1x04_info, },
	{ .compatible = "nvidia,tegra114-host1x", .data = &host1x02_info, },
	{ .compatible = "nvidia,tegra30-host1x", .data = &host1x01_info, },
	{ .compatible = "nvidia,tegra20-host1x", .data = &host1x01_info, },
	{ },
};
MODULE_DEVICE_TABLE(of, host1x_of_match);

static int host1x_probe(struct platform_device *pdev)
{
	struct host1x *host;
	struct resource *regs, *hv_regs = NULL;
	int syncpt_irq;
	int err;

	host = devm_kzalloc(&pdev->dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	mutex_init(&host->devices_lock);
	INIT_LIST_HEAD(&host->devices);
	INIT_LIST_HEAD(&host->list);
	host->info = of_device_get_match_data(&pdev->dev);
	host->dev = &pdev->dev;

	/* set common host1x device data */
	platform_set_drvdata(pdev, host);

	dma_set_mask_and_coherent(host->dev, host->info->dma_mask);

	if (host->info->has_hypervisor) {
		hv_regs = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						       "hypervisor");
		if (!hv_regs) {
			dev_err(&pdev->dev,
				"failed to get hypervisor registers\n");
			return -ENXIO;
		}

		host->hv_regs = devm_ioremap_resource(&pdev->dev, hv_regs);
		if (IS_ERR(host->hv_regs))
			return PTR_ERR(host->hv_regs);

		regs = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vm");
		if (!regs) {
			dev_err(&pdev->dev, "failed to get vm registers\n");
			return -ENXIO;
		}
	} else {
		regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!regs) {
			dev_err(&pdev->dev, "failed to get registers\n");
			return -ENXIO;
		}
	}

	host->regs = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(host->regs))
		return PTR_ERR(host->regs);

	syncpt_irq = platform_get_irq(pdev, 0);
	if (syncpt_irq < 0)
		return syncpt_irq;

	host->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(host->clk)) {
		err = PTR_ERR(host->clk);

		if (err != -EPROBE_DEFER)
			dev_err(&pdev->dev, "failed to get clock: %d\n", err);

		return err;
	}

	host->rst = devm_reset_control_get(&pdev->dev, "host1x");
	if (IS_ERR(host->rst)) {
		err = PTR_ERR(host->rst);
		dev_err(&pdev->dev, "failed to get reset: %d\n", err);
		return err;
	}

	err = clk_prepare_enable(host->clk);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to enable clock\n");
		return err;
	}

	err = reset_control_deassert(host->rst);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to deassert reset: %d\n", err);
		goto fail_unprepare_disable;
	}

	host->debugfs = debugfs_create_dir("tegra-host1x", NULL);

	err = host1x_register(host);
	if (err < 0)
		goto fail_debugfs;

	err = devm_of_platform_populate(&pdev->dev);
	if (err < 0)
		goto unregister;

	return 0;

unregister:
	host1x_unregister(host);
fail_debugfs:
	debugfs_remove_recursive(host->debugfs);
fail_reset_assert:
	reset_control_assert(host->rst);
fail_unprepare_disable:
	clk_disable_unprepare(host->clk);

	return err;
}

static int host1x_remove(struct platform_device *pdev)
{
	struct host1x *host = platform_get_drvdata(pdev);

	host1x_unregister(host);
	debugfs_remove_recursive(host->debugfs);
	reset_control_assert(host->rst);
	clk_disable_unprepare(host->clk);

	return 0;
}

static struct platform_driver tegra_host1x_driver = {
	.driver = {
		.name = "tegra-host1x",
		.of_match_table = host1x_of_match,
	},
	.probe = host1x_probe,
	.remove = host1x_remove,
};

static struct platform_driver * const drivers[] = {
	&tegra_host1x_driver,
	&tegra_mipi_driver,
};

static int __init tegra_host1x_init(void)
{
	int err;

	err = bus_register(&host1x_bus_type);
	if (err < 0)
		return err;

	err = platform_register_drivers(drivers, ARRAY_SIZE(drivers));
	if (err < 0)
		bus_unregister(&host1x_bus_type);

	return err;
}
module_init(tegra_host1x_init);

static void __exit tegra_host1x_exit(void)
{
	platform_unregister_drivers(drivers, ARRAY_SIZE(drivers));
	bus_unregister(&host1x_bus_type);
}
module_exit(tegra_host1x_exit);

MODULE_AUTHOR("Thierry Reding <thierry.reding@avionic-design.de>");
MODULE_AUTHOR("Terje Bergstrom <tbergstrom@nvidia.com>");
MODULE_DESCRIPTION("Host1x driver for Tegra products");
MODULE_LICENSE("GPL");
