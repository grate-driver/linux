// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reboot.h>

static void deassert_pshold(void __iomem *msm_ps_hold)
{
	writel(0, msm_ps_hold);
	mdelay(10000);
}

static void do_msm_restart(struct restart_data *data)
{
	deassert_pshold(data->cb_data);
}

static void do_msm_poweroff(void *msm_ps_hold)
{
	deassert_pshold(msm_ps_hold);
}

static int msm_restart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void __iomem *msm_ps_hold;
	struct resource *mem;
	int err;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	msm_ps_hold = devm_ioremap_resource(dev, mem);
	if (IS_ERR(msm_ps_hold))
		return PTR_ERR(msm_ps_hold);

	err = devm_register_simple_restart_handler(&pdev->dev, do_msm_restart,
						   msm_ps_hold);
	if (err)
		return err;

	err = devm_register_simple_power_off_handler(&pdev->dev, do_msm_poweroff,
						     msm_ps_hold);
	if (err)
		return err;

	return 0;
}

static const struct of_device_id of_msm_restart_match[] = {
	{ .compatible = "qcom,pshold", },
	{},
};
MODULE_DEVICE_TABLE(of, of_msm_restart_match);

static struct platform_driver msm_restart_driver = {
	.probe = msm_restart_probe,
	.driver = {
		.name = "msm-restart",
		.of_match_table = of_match_ptr(of_msm_restart_match),
	},
};

static int __init msm_restart_init(void)
{
	return platform_driver_register(&msm_restart_driver);
}
device_initcall(msm_restart_init);
