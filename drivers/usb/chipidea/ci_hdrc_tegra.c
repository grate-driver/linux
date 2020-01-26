// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016, NVIDIA Corporation
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_opp.h>
#include <linux/reset.h>

#include <linux/usb/chipidea.h>

#include "ci.h"

struct tegra_udc {
	struct ci_hdrc_platform_data data;
	struct platform_device *dev;

	struct usb_phy *phy;
	struct clk *clk;
};

struct tegra_udc_soc_info {
	unsigned long flags;
};

static const struct tegra_udc_soc_info tegra_udc_soc_info = {
	.flags = CI_HDRC_REQUIRES_ALIGNED_DMA,
};

static const struct of_device_id tegra_udc_of_match[] = {
	{
		.compatible = "nvidia,tegra20-udc",
		.data = &tegra_udc_soc_info,
	}, {
		.compatible = "nvidia,tegra30-udc",
		.data = &tegra_udc_soc_info,
	}, {
		.compatible = "nvidia,tegra114-udc",
		.data = &tegra_udc_soc_info,
	}, {
		.compatible = "nvidia,tegra124-udc",
		.data = &tegra_udc_soc_info,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, tegra_udc_of_match);

static void tegra_udc_deinit_opp_table(void *data)
{
	struct device *dev = data;
	struct opp_table *opp_table;

	opp_table = dev_pm_opp_get_opp_table(dev);
	dev_pm_opp_of_remove_table(dev);
	dev_pm_opp_put_regulators(opp_table);
	dev_pm_opp_put_opp_table(opp_table);
}

static int devm_tegra_udc_init_opp_table(struct device *dev)
{
	unsigned long rate = ULONG_MAX;
	struct opp_table *opp_table;
	const char *rname = "core";
	struct dev_pm_opp *opp;
	int err;

	/* legacy device-trees don't have OPP table */
	if (!device_property_present(dev, "operating-points-v2"))
		return 0;

	/* voltage scaling is optional */
	if (device_property_present(dev, "core-supply"))
		opp_table = dev_pm_opp_set_regulators(dev, &rname, 1);
	else
		opp_table = dev_pm_opp_get_opp_table(dev);

	if (IS_ERR(opp_table))
		return dev_err_probe(dev, PTR_ERR(opp_table),
				     "failed to prepare OPP table\n");

	err = dev_pm_opp_of_add_table(dev);
	if (err) {
		dev_err(dev, "failed to add OPP table: %d\n", err);
		goto put_table;
	}

	/* find suitable OPP for the maximum clock rate */
	opp = dev_pm_opp_find_freq_floor(dev, &rate);
	err = PTR_ERR_OR_ZERO(opp);
	if (err) {
		dev_err(dev, "failed to get OPP: %d\n", err);
		goto remove_table;
	}

	dev_pm_opp_put(opp);

	/*
	 * First dummy rate-set initializes voltage vote by setting voltage
	 * in accordance to the clock rate.
	 */
	err = dev_pm_opp_set_rate(dev, rate);
	if (err) {
		dev_err(dev, "failed to initialize OPP clock: %d\n", err);
		goto remove_table;
	}

	err = devm_add_action(dev, tegra_udc_deinit_opp_table, dev);
	if (err)
		goto remove_table;

	return 0;

remove_table:
	dev_pm_opp_of_remove_table(dev);
put_table:
	dev_pm_opp_put_regulators(opp_table);

	return err;
}

static int tegra_udc_probe(struct platform_device *pdev)
{
	const struct tegra_udc_soc_info *soc;
	struct tegra_udc *udc;
	int err;

	udc = devm_kzalloc(&pdev->dev, sizeof(*udc), GFP_KERNEL);
	if (!udc)
		return -ENOMEM;

	soc = of_device_get_match_data(&pdev->dev);
	if (!soc) {
		dev_err(&pdev->dev, "failed to match OF data\n");
		return -EINVAL;
	}

	udc->phy = devm_usb_get_phy_by_phandle(&pdev->dev, "nvidia,phy", 0);
	if (IS_ERR(udc->phy)) {
		err = PTR_ERR(udc->phy);
		dev_err(&pdev->dev, "failed to get PHY: %d\n", err);
		return err;
	}

	udc->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(udc->clk)) {
		err = PTR_ERR(udc->clk);
		dev_err(&pdev->dev, "failed to get clock: %d\n", err);
		return err;
	}

	err = devm_tegra_udc_init_opp_table(&pdev->dev);
	if (err)
		return dev_err_probe(&pdev->dev, err,
				     "failed to initialize OPP\n");

	err = clk_prepare_enable(udc->clk);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to enable clock: %d\n", err);
		return err;
	}

	/* setup and register ChipIdea HDRC device */
	udc->data.name = "tegra-udc";
	udc->data.flags = soc->flags;
	udc->data.usb_phy = udc->phy;
	udc->data.capoffset = DEF_CAPOFFSET;

	/* check the dual mode and warn about bad configurations */
	if (usb_get_dr_mode(&pdev->dev) == USB_DR_MODE_OTG &&
	   !of_property_read_bool(pdev->dev.of_node, "extcon")) {
		dev_warn(&pdev->dev, "no extcon registered, otg unavailable");
		udc->data.flags |= CI_HDRC_DUAL_ROLE_NOT_OTG;
	}

	udc->dev = ci_hdrc_add_device(&pdev->dev, pdev->resource,
				      pdev->num_resources, &udc->data);
	if (IS_ERR(udc->dev)) {
		err = PTR_ERR(udc->dev);
		dev_err(&pdev->dev, "failed to add HDRC device: %d\n", err);
		goto fail_power_off;
	}

	platform_set_drvdata(pdev, udc);

	return 0;

fail_power_off:
	clk_disable_unprepare(udc->clk);
	return err;
}

static int tegra_udc_remove(struct platform_device *pdev)
{
	struct tegra_udc *udc = platform_get_drvdata(pdev);

	ci_hdrc_remove_device(udc->dev);
	clk_disable_unprepare(udc->clk);

	return 0;
}

static struct platform_driver tegra_udc_driver = {
	.driver = {
		.name = "tegra-udc",
		.of_match_table = tegra_udc_of_match,
	},
	.probe = tegra_udc_probe,
	.remove = tegra_udc_remove,
};
module_platform_driver(tegra_udc_driver);

MODULE_DESCRIPTION("NVIDIA Tegra USB device mode driver");
MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_ALIAS("platform:tegra-udc");
MODULE_LICENSE("GPL v2");
