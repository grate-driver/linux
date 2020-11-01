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
	struct tegra_drm_client client;
	struct host1x_channel *channel;
	struct clk *clk;

	const struct gr2d_soc *soc;

	DECLARE_BITMAP(addr_regs, GR2D_NUM_REGS);
};

static inline struct gr2d *to_gr2d(struct tegra_drm_client *client)
{
	return container_of(client, struct gr2d, client);
}

static int gr2d_init(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct drm_device *dev = dev_get_drvdata(client->host);
	unsigned long flags = HOST1X_SYNCPT_HAS_BASE;
	struct gr2d *gr2d = to_gr2d(drm);
	int err;

	gr2d->channel = host1x_channel_request(client);
	if (!gr2d->channel)
		return -ENOMEM;

	client->syncpts[0] = host1x_syncpt_request(client, flags);
	if (!client->syncpts[0]) {
		err = -ENOMEM;
		dev_err(client->dev, "failed to request syncpoint: %d\n", err);
		goto put;
	}

	err = host1x_client_iommu_attach(client);
	if (err < 0) {
		dev_err(client->dev, "failed to attach to domain: %d\n", err);
		goto free;
	}

	err = tegra_drm_register_client(dev->dev_private, drm);
	if (err < 0) {
		dev_err(client->dev, "failed to register client: %d\n", err);
		goto detach;
	}

	return 0;

detach:
	host1x_client_iommu_detach(client);
free:
	host1x_syncpt_free(client->syncpts[0]);
put:
	host1x_channel_put(gr2d->channel);
	return err;
}

static int gr2d_exit(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct drm_device *dev = dev_get_drvdata(client->host);
	struct tegra_drm *tegra = dev->dev_private;
	struct gr2d *gr2d = to_gr2d(drm);
	int err;

	err = tegra_drm_unregister_client(tegra, drm);
	if (err < 0)
		return err;

	host1x_client_iommu_detach(client);
	host1x_syncpt_free(client->syncpts[0]);
	host1x_channel_put(gr2d->channel);

	return 0;
}

static const struct host1x_client_ops gr2d_client_ops = {
	.init = gr2d_init,
	.exit = gr2d_exit,
};

static int gr2d_open_channel(struct tegra_drm_client *client,
			     struct tegra_drm_context *context)
{
	struct gr2d *gr2d = to_gr2d(client);

	context->channel = host1x_channel_get(gr2d->channel);
	if (!context->channel)
		return -ENOMEM;

	return 0;
}

static void gr2d_close_channel(struct tegra_drm_context *context)
{
	host1x_channel_put(context->channel);
}

static int gr2d_is_addr_reg(struct device *dev, u32 class, u32 offset)
{
	struct gr2d *gr2d = dev_get_drvdata(dev);

	switch (class) {
	case HOST1X_CLASS_HOST1X:
		if (offset == 0x2b)
			return 1;

		break;

	case HOST1X_CLASS_GR2D:
	case HOST1X_CLASS_GR2D_SB:
		if (offset >= GR2D_NUM_REGS)
			break;

		if (test_bit(offset, gr2d->addr_regs))
			return 1;

		break;
	}

	return 0;
}

static int gr2d_is_valid_class(u32 class)
{
	return (class == HOST1X_CLASS_GR2D ||
		class == HOST1X_CLASS_GR2D_SB);
}

static const struct tegra_drm_client_ops gr2d_ops = {
	.open_channel = gr2d_open_channel,
	.close_channel = gr2d_close_channel,
	.is_addr_reg = gr2d_is_addr_reg,
	.is_valid_class = gr2d_is_valid_class,
	.submit = tegra_drm_submit,
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

static const u32 gr2d_addr_regs[] = {
	GR2D_UA_BASE_ADDR,
	GR2D_VA_BASE_ADDR,
	GR2D_PAT_BASE_ADDR,
	GR2D_DSTA_BASE_ADDR,
	GR2D_DSTB_BASE_ADDR,
	GR2D_DSTC_BASE_ADDR,
	GR2D_SRCA_BASE_ADDR,
	GR2D_SRCB_BASE_ADDR,
	GR2D_PATBASE_ADDR,
	GR2D_SRC_BASE_ADDR_SB,
	GR2D_DSTA_BASE_ADDR_SB,
	GR2D_DSTB_BASE_ADDR_SB,
	GR2D_UA_BASE_ADDR_SB,
	GR2D_VA_BASE_ADDR_SB,
};

static void gr2d_pm_runtime_release(void *dev)
{
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
}

static int gr2d_probe(struct platform_device *pdev)
{
	struct tegra_core_opp_params opp_params = {};
	struct device *dev = &pdev->dev;
	struct host1x_syncpt **syncpts;
	struct gr2d *gr2d;
	unsigned int i;
	int err;

	gr2d = devm_kzalloc(dev, sizeof(*gr2d), GFP_KERNEL);
	if (!gr2d)
		return -ENOMEM;

	platform_set_drvdata(pdev, gr2d);

	gr2d->soc = of_device_get_match_data(dev);

	syncpts = devm_kzalloc(dev, sizeof(*syncpts), GFP_KERNEL);
	if (!syncpts)
		return -ENOMEM;

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

	INIT_LIST_HEAD(&gr2d->client.base.list);
	gr2d->client.base.ops = &gr2d_client_ops;
	gr2d->client.base.dev = dev;
	gr2d->client.base.class = HOST1X_CLASS_GR2D;
	gr2d->client.base.syncpts = syncpts;
	gr2d->client.base.num_syncpts = 1;

	INIT_LIST_HEAD(&gr2d->client.list);
	gr2d->client.version = gr2d->soc->version;
	gr2d->client.ops = &gr2d_ops;

	err = host1x_client_register(&gr2d->client.base);
	if (err < 0) {
		dev_err(dev, "failed to register host1x client: %d\n", err);
		return err;
	}

	/* initialize address register map */
	for (i = 0; i < ARRAY_SIZE(gr2d_addr_regs); i++)
		set_bit(gr2d_addr_regs[i], gr2d->addr_regs);

	return 0;
}

static int gr2d_remove(struct platform_device *pdev)
{
	struct gr2d *gr2d = platform_get_drvdata(pdev);
	int err;

	err = host1x_client_unregister(&gr2d->client.base);
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
	struct gr2d *gr2d = dev_get_drvdata(dev);
	int err;

	host1x_channel_stop(gr2d->channel);

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
