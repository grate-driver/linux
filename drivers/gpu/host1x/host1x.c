/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <soc/tegra/common.h>

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

	pm_runtime_enable(host->dev);
	err = pm_runtime_resume_and_get(host->dev);
	if (err < 0) {
		pm_runtime_disable(host->dev);
		return err;
	}

	reset_control_release(host->rst);

	return 0;
}

static void host1x_deinit_hw(struct host1x *host)
{
	reset_control_assert(host->rst);

	usleep_range(1000, 2000);

	pm_runtime_put(host->dev);
	pm_runtime_disable(host->dev);
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
		goto deinit_hw;

	err = host1x_init_dma_pool(host);
	if (err)
		goto deinit_iommu;

	err = host1x_init_syncpts(host);
	if (err)
		goto deinit_dma_pool;

	err = host1x_init_mlocks(host);
	if (err)
		goto deinit_syncpoints;

	err = host1x_init_channels(host);
	if (err)
		goto deinit_mlocks;

	err = host1x_init_debug(host);
	if (err)
		goto deinit_channels;

	err = host1x_register(host);
	if (err)
		goto deinit_debug;

	err = devm_of_platform_populate(host->dev);
	if (err)
		goto unregister;

	return 0;

unregister:
	host1x_unregister(host);
deinit_debug:
	host1x_deinit_debug(host);
deinit_channels:
	host1x_deinit_channels(host);
deinit_mlocks:
	host1x_deinit_mlocks(host);
deinit_syncpoints:
	host1x_deinit_syncpts(host);
deinit_dma_pool:
	host1x_deinit_dma_pool(host);
deinit_iommu:
	host1x_deinit_iommu(host);
deinit_hw:
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

	host->rst = devm_reset_control_get_exclusive_released(&pdev->dev, "host1x");
	if (IS_ERR(host->rst)) {
		err = PTR_ERR(host->rst);
		dev_err(&pdev->dev, "failed to get reset: %d\n", err);
		return err;
	}

	err = host1x_init(host);
	if (err)
		return dev_err_probe(&pdev->dev, err,
				     "initialization failed\n");

	pm_runtime_put(&pdev->dev);

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

static int __maybe_unused host1x_runtime_suspend(struct device *dev)
{
	struct host1x *host = dev_get_drvdata(dev);

	clk_disable_unprepare(host->clk);
	reset_control_release(host->rst);

	return 0;
}

static int __maybe_unused host1x_runtime_resume(struct device *dev)
{
	struct host1x *host = dev_get_drvdata(dev);
	int err;

	err = reset_control_acquire(host->rst);
	if (err) {
		dev_err(dev, "failed to acquire reset: %d\n", err);
		return err;
	}

	err = clk_prepare_enable(host->clk);
	if (err) {
		dev_err(dev, "failed to enable clock: %d\n", err);
		goto release_reset;
	}

	return 0;

release_reset:
	reset_control_release(host->rst);

	return err;
}

static const struct dev_pm_ops host1x_pm = {
	SET_RUNTIME_PM_OPS(host1x_runtime_suspend, host1x_runtime_resume,
			   NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static struct platform_driver tegra_host1x_driver = {
	.driver = {
		.name = "tegra-host1x",
		.of_match_table = host1x_of_match,
		.pm = &host1x_pm,
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
