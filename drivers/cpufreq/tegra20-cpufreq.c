// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *	Based on arch/arm/plat-omap/cpu-omap.c, (C) 2005 Nokia Corporation
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>

#include <soc/tegra/common.h>
#include <soc/tegra/fuse.h>

/* PLLP is a main system PLL which runs at a static rate all the time */
static unsigned long pllp_rate;

static int tegra20_cpufreq_set_voltage(struct dev_pm_set_opp_data *data)
{
	struct dev_pm_opp_supply *supply = data->new_opp.supplies;
	int ret;

	if (!data->regulators)
		return 0;

	ret = regulator_set_voltage_triplet(data->regulators[0],
					    supply->u_volt_min, supply->u_volt,
					    supply->u_volt_max);
	if (ret)
		dev_err(data->dev, "failed to set voltage (%lu %lu %lu mV): %d\n",
			supply->u_volt_min, supply->u_volt,
			supply->u_volt_max, ret);

	return ret;
}

static int tegra20_cpufreq_set_opp(struct dev_pm_set_opp_data *data)
{
	int err;

	/*
	 * All Tegra SoCs require an intermediate step for CPU clock rate
	 * transition. The clk driver takes care of switching the CPU clock
	 * to a backup parent during transition. But if there is a need to
	 * change CPU voltage for the transition, then going from a low freq
	 * to a high freq may take dozen milliseconds, which could be
	 * unacceptably long transition time for some applications which
	 * require CPU to run at a reasonable performance immediately.
	 *
	 * In order to mitigate the potentially long transition time, we
	 * will switch CPU to a faster backup freq upfront, i.e. before
	 * the voltage change is initiated.
	 */
	if (data->old_opp.rate < pllp_rate &&
	    data->new_opp.rate > pllp_rate) {
		err = clk_set_rate(data->clk, pllp_rate);
		if (err) {
			dev_err(data->dev,
				"failed to set backup clock rate: %d\n", err);
			return err;
		}
	}

	if (data->new_opp.rate > data->old_opp.rate) {
		err = tegra20_cpufreq_set_voltage(data);
		if (err)
			return err;
	}

	err = clk_set_rate(data->clk, data->new_opp.rate);
	if (err) {
		dev_err(data->dev, "failed to set clock rate: %d\n", err);
		return err;
	}

	if (data->new_opp.rate < data->old_opp.rate) {
		err = tegra20_cpufreq_set_voltage(data);
		if (err)
			return err;
	}

	return 0;
}

static bool cpu0_node_has_opp_v2_prop(void)
{
	struct device_node *np = of_cpu_device_node_get(0);
	bool ret = false;

	if (of_get_property(np, "operating-points-v2", NULL))
		ret = true;

	of_node_put(np);
	return ret;
}

static void tegra20_cpufreq_unregister_opp_helper(void *opp_table)
{
	dev_pm_opp_unregister_set_opp_helper(opp_table);
}

static void tegra20_cpufreq_put_supported_hw(void *opp_table)
{
	dev_pm_opp_put_supported_hw(opp_table);
}

static void tegra20_cpufreq_dt_unregister(void *cpufreq_dt)
{
	platform_device_unregister(cpufreq_dt);
}

static int tegra20_cpufreq_probe(struct platform_device *pdev)
{
	struct platform_device *cpufreq_dt;
	struct opp_table *opp_table;
	struct device *cpu_dev;
	struct clk *pllp;
	u32 versions[2];
	int err;

	if (!cpu0_node_has_opp_v2_prop()) {
		dev_err(&pdev->dev, "operating points not found\n");
		dev_err(&pdev->dev, "please update your device tree\n");
		return -ENODEV;
	}

	if (of_machine_is_compatible("nvidia,tegra20")) {
		versions[0] = BIT(tegra_sku_info.cpu_process_id);
		versions[1] = BIT(tegra_sku_info.soc_speedo_id);
	} else {
		versions[0] = BIT(tegra_sku_info.cpu_process_id);
		versions[1] = BIT(tegra_sku_info.cpu_speedo_id);
	}

	dev_info(&pdev->dev, "hardware version 0x%x 0x%x\n",
		 versions[0], versions[1]);

	pllp = clk_get_sys(NULL, "pll_p");
	if (IS_ERR(pllp)) {
		dev_err(&pdev->dev, "failed to get PLLP: %pe\n", pllp);
		return PTR_ERR(pllp);
	}
	pllp_rate = clk_get_rate(pllp);
	clk_put(pllp);

	cpu_dev = get_cpu_device(0);
	if (WARN_ON(!cpu_dev))
		return -ENODEV;

	opp_table = dev_pm_opp_set_supported_hw(cpu_dev, versions, 2);
	err = PTR_ERR_OR_ZERO(opp_table);
	if (err) {
		dev_err(&pdev->dev, "failed to set supported hw: %d\n", err);
		return err;
	}

	err = devm_add_action_or_reset(&pdev->dev,
				       tegra20_cpufreq_put_supported_hw,
				       opp_table);
	if (err)
		return err;

	opp_table = dev_pm_opp_register_set_opp_helper(cpu_dev,
						       tegra20_cpufreq_set_opp);
	if (IS_ERR(opp_table))
		return PTR_ERR(opp_table);

	err = devm_add_action_or_reset(&pdev->dev,
				       tegra20_cpufreq_unregister_opp_helper,
				       opp_table);
	if (err)
		return err;

	cpufreq_dt = platform_device_register_simple("cpufreq-dt", -1, NULL, 0);
	err = PTR_ERR_OR_ZERO(cpufreq_dt);
	if (err) {
		dev_err(&pdev->dev,
			"failed to create cpufreq-dt device: %d\n", err);
		return err;
	}

	err = devm_add_action_or_reset(&pdev->dev,
				       tegra20_cpufreq_dt_unregister,
				       cpufreq_dt);
	if (err)
		return err;

	return 0;
}

static struct platform_driver tegra20_cpufreq_driver = {
	.probe		= tegra20_cpufreq_probe,
	.driver		= {
		.name	= "tegra20-cpufreq",
	},
};
module_platform_driver(tegra20_cpufreq_driver);

MODULE_ALIAS("platform:tegra20-cpufreq");
MODULE_AUTHOR("Colin Cross <ccross@android.com>");
MODULE_DESCRIPTION("NVIDIA Tegra20 cpufreq driver");
MODULE_LICENSE("GPL");
