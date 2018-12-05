/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "bus.h"
#include "host1x.h"

#include "soc/host1x01.h"
#include "soc/host1x02.h"
#include "soc/host1x04.h"
#include "soc/host1x05.h"
#include "soc/host1x06.h"
#include "soc/host1x07.h"

static int host1x_init_hw(struct host1x *host)
{
	int err;

	err = clk_prepare_enable(host->clk);
	if (err) {
		dev_err(host->dev, "failed to enable clock: %d\n", err);
		return err;
	}

	err = reset_control_deassert(host->rst);
	if (err) {
		dev_err(host->dev, "failed to deassert reset: %d\n", err);
		goto err_disable_clk;
	}

	return 0;

err_disable_clk:
	clk_disable_unprepare(host->clk);

	return err;
}

static void host1x_deinit_hw(struct host1x *host)
{
	reset_control_assert(host->rst);

	usleep_range(1000, 2000);

	clk_disable_unprepare(host->clk);
}

static int host1x_init(struct host1x *host)
{
	int err;

	err = host->soc->init_ops(host);
	if (err)
		return err;

	err = host1x_init_hw(host);
	if (err)
		return err;

	err = host1x_init_iommu(host);
	if (err)
		goto err_deinit_hw;

	err = host1x_init_dma_pool(host);
	if (err)
		goto err_deinit_iommu;

	err = host1x_init_syncpts(host);
	if (err)
		goto err_deinit_dma_pool;

	err = host1x_init_mlocks(host);
	if (err)
		goto err_deinit_syncpoints;

	err = host1x_init_channels(host);
	if (err)
		goto err_deinit_mlocks;

	err = host1x_init_debug(host);
	if (err)
		goto err_deinit_channels;

	err = host1x_register(host);
	if (err)
		goto err_deinit_debug;

	return 0;

err_deinit_debug:
	host1x_deinit_debug(host);

err_deinit_channels:
	host1x_deinit_channels(host);

err_deinit_mlocks:
	host1x_deinit_mlocks(host);

err_deinit_syncpoints:
	host1x_deinit_syncpts(host);

err_deinit_dma_pool:
	host1x_deinit_dma_pool(host);

err_deinit_iommu:
	host1x_deinit_iommu(host);

err_deinit_hw:
	host1x_deinit_hw(host);

	return err;
}

static int host1x_probe(struct platform_device *pdev)
{
	const struct host1x_soc *soc;
	struct resource *res;
	struct host1x *host;
	int err;

	soc = of_device_get_match_data(&pdev->dev);
	dma_set_mask_and_coherent(&pdev->dev, soc->dma_mask);

	host = devm_kzalloc(&pdev->dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	platform_set_drvdata(pdev, host);

	if (soc->has_hypervisor) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		host->hv_regs = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(host->hv_regs))
			return PTR_ERR(host->hv_regs);

		res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		host->base_regs = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(host->base_regs))
			return PTR_ERR(host->base_regs);
	} else {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		host->base_regs = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(host->base_regs))
			return PTR_ERR(host->base_regs);
	}

	err = platform_get_irq(pdev, 0);
	if (err < 0)
		return err;

	host->soc = soc;
	host->dev = &pdev->dev;
	host->syncpt_irq = err;

	host->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(host->clk)) {
		err = PTR_ERR(host->clk);
		dev_err(&pdev->dev, "failed to get clock: %d\n", err);
		return err;
	}

	host->rst = devm_reset_control_get(&pdev->dev, "host1x");
	if (IS_ERR(host->rst)) {
		err = PTR_ERR(host->rst);
		dev_err(&pdev->dev, "failed to get reset: %d\n", err);
		return err;
	}

	err = host1x_init(host);
	if (err) {
		dev_err(&pdev->dev, "initialization failed: %d\n", err);
		return err;
	}

	return 0;
}

static int host1x_remove(struct platform_device *pdev)
{
	struct host1x *host = platform_get_drvdata(pdev);

	host1x_unregister(host);
	host1x_deinit_debug(host);
	host1x_deinit_channels(host);
	host1x_deinit_mlocks(host);
	host1x_deinit_syncpts(host);
	host1x_deinit_dma_pool(host);
	host1x_deinit_iommu(host);
	host1x_deinit_hw(host);

	return 0;
}

static const struct host1x_soc host1x01_soc = {
	.nb_channels	= 8,
	.nb_syncpts	= 32,
	.nb_mlocks	= 16,
	.nb_bases	= 8,
	.dma_mask	= DMA_BIT_MASK(32),
	.has_hypervisor	= false,
	.nb_sid_entries	= 0,
	.sid_table	= NULL,
	.init_ops	= host1x01_init,
};

static const struct host1x_soc host1x02_soc = {
	.nb_channels	= 9,
	.nb_syncpts	= 32,
	.nb_mlocks	= 16,
	.nb_bases	= 12,
	.dma_mask	= DMA_BIT_MASK(32),
	.has_hypervisor	= false,
	.nb_sid_entries	= 0,
	.sid_table	= NULL,
	.init_ops	= host1x02_init,
};

static const struct host1x_soc host1x04_soc = {
	.nb_channels	= 12,
	.nb_syncpts	= 192,
	.nb_mlocks	= 16,
	.nb_bases	= 64,
	.dma_mask	= DMA_BIT_MASK(34),
	.has_hypervisor	= false,
	.nb_sid_entries	= 0,
	.sid_table	= NULL,
	.init_ops	= host1x04_init,
};

static const struct host1x_soc host1x05_soc = {
	.nb_channels	= 14,
	.nb_syncpts	= 192,
	.nb_mlocks	= 16,
	.nb_bases	= 64,
	.dma_mask	= DMA_BIT_MASK(34),
	.has_hypervisor	= false,
	.nb_sid_entries	= 0,
	.sid_table	= NULL,
	.init_ops	= host1x05_init,
};

static const struct host1x_sid_entry tegra186_sid_table[] = {
	{
		/* VIC */
		.base	= 0x1af0,
		.offset	= 0x30,
		.limit	= 0x34
	},
};

static const struct host1x_soc host1x06_soc = {
	.nb_channels	= 63,
	.nb_syncpts	= 576,
	.nb_mlocks	= 24,
	.nb_bases	= 16,
	.dma_mask	= DMA_BIT_MASK(40),
	.has_hypervisor	= true,
	.nb_sid_entries	= ARRAY_SIZE(tegra186_sid_table),
	.sid_table	= tegra186_sid_table,
	.init_ops	= host1x06_init,
};

static const struct host1x_sid_entry tegra194_sid_table[] = {
	{
		/* VIC */
		.base	= 0x1af0,
		.offset	= 0x30,
		.limit	= 0x34
	},
};

static const struct host1x_soc host1x07_soc = {
	.nb_channels	= 63,
	.nb_syncpts	= 704,
	.nb_mlocks	= 32,
	.nb_bases	= 0,
	.dma_mask	= DMA_BIT_MASK(40),
	.has_hypervisor	= true,
	.nb_sid_entries	= ARRAY_SIZE(tegra194_sid_table),
	.sid_table	= tegra194_sid_table,
	.init_ops	= host1x07_init,
};

static const struct of_device_id host1x_of_match[] = {
	{ .compatible = "nvidia,tegra194-host1x", .data = &host1x07_soc, },
	{ .compatible = "nvidia,tegra186-host1x", .data = &host1x06_soc, },
	{ .compatible = "nvidia,tegra210-host1x", .data = &host1x05_soc, },
	{ .compatible = "nvidia,tegra124-host1x", .data = &host1x04_soc, },
	{ .compatible = "nvidia,tegra114-host1x", .data = &host1x02_soc, },
	{ .compatible = "nvidia,tegra30-host1x",  .data = &host1x01_soc, },
	{ .compatible = "nvidia,tegra20-host1x",  .data = &host1x01_soc, },
	{ },
};
MODULE_DEVICE_TABLE(of, host1x_of_match);

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

static int __init host1x_module_init(void)
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
module_init(host1x_module_init);

static void __exit host1x_module_exit(void)
{
	platform_unregister_drivers(drivers, ARRAY_SIZE(drivers));
	bus_unregister(&host1x_bus_type);
}
module_exit(host1x_module_exit);

MODULE_AUTHOR("Thierry Reding <thierry.reding@avionic-design.de>");
MODULE_AUTHOR("Terje Bergstrom <tbergstrom@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra HOST1x driver");
MODULE_LICENSE("GPL");
