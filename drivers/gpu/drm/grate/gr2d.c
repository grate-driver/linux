// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2013, NVIDIA Corporation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <drm/gpu_scheduler.h>

#include <soc/tegra/common.h>

#include "drm.h"
#include "job.h"
#include "gr2d.h"

#define OPCODE_SETCL(classid, offset, mask)		\
	((0 << 28) | (offset << 16) | (classid << 6) | mask)

#define OPCODE_INCR(offset, count)			\
	((1 << 28) | (offset << 16) | count)

#define RESET_TRIG	0x0
#define RESET_ADDR	TEGRA_POISON_ADDR

struct gr2d_soc {
	unsigned int version;
};

struct gr2d {
	struct iommu_group *group;
	struct reset_control *reset;
	struct reset_control *reset_mc;
	struct tegra_drm_client client;
	struct tegra_drm_channel *channel;
	struct host1x_gather init_gather;
	struct clk *clk;

	const struct gr2d_soc *soc;

	DECLARE_BITMAP(addr_regs, GR2D_NUM_REGS);
};

static const struct gr2d_soc tegra20_gr2d_soc = {
	.version = 0x20,
};

static const struct gr2d_soc tegra30_gr2d_soc = {
	.version = 0x30,
};

static const struct gr2d_soc tegra114_gr2d_soc = {
	.version = 0x35,
};

static const struct of_device_id gr2d_match[] = {
	{ .compatible = "nvidia,tegra114-gr2d", .data = &tegra114_gr2d_soc },
	{ .compatible = "nvidia,tegra30-gr2d", .data = &tegra30_gr2d_soc },
	{ .compatible = "nvidia,tegra20-gr2d", .data = &tegra20_gr2d_soc },
	{ },
};
MODULE_DEVICE_TABLE(of, gr2d_match);

static const u32 gr2d_hw_init[] = {
	OPCODE_SETCL(HOST1X_CLASS_GR2D_G2_1_CTX1, GR2D_G2TRIGGER0, 0x7),
	RESET_TRIG, RESET_TRIG, RESET_TRIG,
	OPCODE_INCR(GR2D_DSTA_BASE_ADDR, 3),
	RESET_ADDR, RESET_ADDR, RESET_ADDR,

	OPCODE_SETCL(HOST1X_CLASS_GR2D_G2_1_CTX2, GR2D_G2TRIGGER0, 0x7),
	RESET_TRIG, RESET_TRIG, RESET_TRIG,
	OPCODE_INCR(GR2D_DSTA_BASE_ADDR, 3),
	RESET_ADDR, RESET_ADDR, RESET_ADDR,

	OPCODE_SETCL(HOST1X_CLASS_GR2D_SB_CTX1, GR2D_G2TRIGGER0, 0x7),
	RESET_TRIG, RESET_TRIG, RESET_TRIG,
	OPCODE_INCR(GR2D_DSTA_BASE_ADDR, 3),
	RESET_ADDR, RESET_ADDR, RESET_ADDR,
	OPCODE_INCR(GR2D_DSTA_BASE_ADDR_SB, 2),
	RESET_ADDR, RESET_ADDR,

	OPCODE_SETCL(HOST1X_CLASS_GR2D_SB_CTX2, GR2D_G2TRIGGER0, 0x7),
	RESET_TRIG, RESET_TRIG, RESET_TRIG,
	OPCODE_INCR(GR2D_DSTA_BASE_ADDR, 3),
	RESET_ADDR, RESET_ADDR, RESET_ADDR,
	OPCODE_INCR(GR2D_DSTA_BASE_ADDR_SB, 2),
	RESET_ADDR, RESET_ADDR,
};

static inline struct gr2d *to_gr2d(struct tegra_drm_client *client)
{
	return container_of(client, struct gr2d, client);
}

static int gr2d_init(struct host1x_client *client)
{
	struct tegra_drm_client *drm_client = to_tegra_drm_client(client);
	struct drm_device *drm = dev_get_drvdata(client->host);
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct tegra_drm *tegra_drm = drm->dev_private;
	struct gr2d *gr2d = to_gr2d(drm_client);
	int err;

	gr2d->group = tegra_drm_client_iommu_attach(drm_client, false);
	if (IS_ERR(gr2d->group)) {
		err = PTR_ERR(gr2d->group);
		dev_err(client->dev, "failed to attach to domain: %d\n", err);
		return err;
	}

	err = tegra_drm_register_client(tegra_drm, drm_client);
	if (err) {
		dev_err(client->dev, "failed to register client: %d\n", err);
		goto detach_iommu;
	}

	gr2d->channel = tegra_drm_open_channel(tegra_drm, drm_client,
					       TEGRA_DRM_PIPE_2D,
					       128, 3, 0, 600, "2d channel");
	if (IS_ERR(gr2d->channel)) {
		err = PTR_ERR(gr2d->channel);
		dev_err(client->dev, "failed to open channel: %d\n", err);
		goto unreg_client;
	}

	gr2d->init_gather.bo = host1x_bo_alloc(host, sizeof(gr2d_hw_init),
					       true);
	if (!gr2d->init_gather.bo) {
		err = -ENOMEM;
		dev_err(client->dev, "failed to allocate init bo\n");
		goto close_channel;
	}

	gr2d->init_gather.num_words = ARRAY_SIZE(gr2d_hw_init);

	memcpy(gr2d->init_gather.bo->vaddr, gr2d_hw_init,
	       sizeof(gr2d_hw_init));

	return 0;

close_channel:
	tegra_drm_close_channel(gr2d->channel);

unreg_client:
	tegra_drm_unregister_client(drm_client);

detach_iommu:
	tegra_drm_client_iommu_detach(drm_client, gr2d->group, false);

	return err;
}

static int gr2d_exit(struct host1x_client *client)
{
	struct tegra_drm_client *drm_client = to_tegra_drm_client(client);
	struct drm_device *drm = dev_get_drvdata(client->host);
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct gr2d *gr2d = to_gr2d(drm_client);

	tegra_drm_close_channel(gr2d->channel);
	tegra_drm_unregister_client(drm_client);
	tegra_drm_client_iommu_detach(drm_client, gr2d->group, false);
	host1x_bo_free(host, gr2d->init_gather.bo);

	return 0;
}

static const struct host1x_client_ops gr2d_host1x_client_ops = {
	.init = gr2d_init,
	.exit = gr2d_exit,
};

static const u16 gr2d_addr_regs[] = {
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

static int
gr2d_refine_class(struct tegra_drm_client *client, u64 pipes,
		  unsigned int *classid)
{
	enum drm_tegra_cmdstream_class drm_class = *classid;

	if (!(pipes & TEGRA_DRM_PIPE_2D))
		return -EINVAL;

	/*
	 * Each 2D context has its own sync point client. This allows
	 * to implement lock-less multi-channel 2d job submission, which
	 * eliminates the need to mess with client's MLOCK'ing. Currently
	 * there are two variants of the job:
	 *   1) 2d-only
	 *   2) 2d/3d mix
	 *
	 * 3d channel allow to execute 2d operations and hence there are
	 * two channels that can execute 2d job. Here we are assigning 2D
	 * context per channel.
	 */
	switch (drm_class) {
	case DRM_TEGRA_CMDSTREAM_CLASS_GR2D_G2:
		if (pipes & TEGRA_DRM_PIPE_3D)
			*classid = HOST1X_CLASS_GR2D_G2_1_CTX1;
		else
			*classid = HOST1X_CLASS_GR2D_G2_1_CTX2;
		break;

	case DRM_TEGRA_CMDSTREAM_CLASS_GR2D_SB:
		if (pipes & TEGRA_DRM_PIPE_3D)
			*classid = HOST1X_CLASS_GR2D_SB_CTX1;
		else
			*classid = HOST1X_CLASS_GR2D_SB_CTX2;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int
gr2d_prepare_job(struct tegra_drm_client *client, struct tegra_drm_job *job)
{
	struct gr2d *gr2d = to_gr2d(client);
	int err;

	err = pm_runtime_resume_and_get(client->base.dev);
	if (err < 0)
		return err;

	host1x_job_add_init_gather(&job->base, &gr2d->init_gather);

	return 0;
}

static int
gr2d_unprepare_job(struct tegra_drm_client *client, struct tegra_drm_job *job)
{
	pm_runtime_mark_last_busy(client->base.dev);
	pm_runtime_put_autosuspend(client->base.dev);

	return 0;
}

static int gr2d_reset_hw(struct tegra_drm_client *drm_client)
{
	struct host1x_client *client = &drm_client->base;
	struct gr2d *gr2d = to_gr2d(drm_client);
	int err;

	err = reset_control_assert(gr2d->reset);
	if (err) {
		dev_err(client->dev, "failed to assert reset: %d\n", err);
		return err;
	}

	usleep_range(10, 20);

	err = reset_control_deassert(gr2d->reset);
	if (err) {
		dev_err(client->dev, "failed to deassert reset: %d\n", err);
		return err;
	}

	return 0;
}

static void gr2d_pm_runtime_release(void *dev)
{
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
}

static struct reset_control *devm_gr2d_get_reset(struct device *dev)
{
	struct reset_control *reset;
	int err;

	reset = of_reset_control_array_get_exclusive_released(dev->of_node);
	if (IS_ERR(reset))
		return reset;

	/* TODO: implement devm_reset_control_array_get_exclusive_released() */
	err = devm_add_action_or_reset(dev, (void *)reset_control_put, reset);
	if (err)
		return ERR_PTR(err);

	return reset;
}

static int gr2d_probe(struct platform_device *pdev)
{
	struct tegra_core_opp_params opp_params = {};
	struct device *dev = &pdev->dev;
	struct gr2d *gr2d;
	unsigned int i;
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

	err = devm_tegra_core_dev_init_opp_table(dev, &opp_params);
	if (err && err != -ENODEV)
		return err;

	gr2d->reset = devm_gr2d_get_reset(dev);
	if (IS_ERR(gr2d->reset)) {
		dev_err(dev, "failed to get reset: %pe\n", gr2d->reset);
		return PTR_ERR(gr2d->reset);
	}

	gr2d->reset_mc = devm_reset_control_get_optional_exclusive_released(dev, "mc");
	if (IS_ERR(gr2d->reset_mc)) {
		dev_err(dev, "failed to get MC reset: %pe\n", gr2d->reset_mc);
		return PTR_ERR(gr2d->reset_mc);
	}

	pm_runtime_enable(dev);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 200);

	err = devm_add_action_or_reset(dev, gr2d_pm_runtime_release, dev);
	if (err)
		return err;

	INIT_LIST_HEAD(&gr2d->client.base.list);
	gr2d->client.base.dev = dev;
	gr2d->client.base.ops = &gr2d_host1x_client_ops;
	gr2d->client.base.class = HOST1X_CLASS_GR2D_G2_1_CTX1;

	/* initialize address register map */
	for (i = 0; i < ARRAY_SIZE(gr2d_addr_regs); i++)
		set_bit(gr2d_addr_regs[i], gr2d->addr_regs);

	gr2d->client.refine_class = gr2d_refine_class;
	gr2d->client.prepare_job = gr2d_prepare_job;
	gr2d->client.unprepare_job = gr2d_unprepare_job;
	gr2d->client.reset_hw = gr2d_reset_hw;
	gr2d->client.addr_regs = gr2d->addr_regs;
	gr2d->client.num_regs = GR2D_NUM_REGS;
	gr2d->client.pipe = TEGRA_DRM_PIPE_2D;

	err = host1x_client_register(&gr2d->client.base);
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
	int err;

	drm_sched_stop(&gr2d->channel->sched, NULL);
	host1x_channel_stop(gr2d->channel->channel);
	reset_control_release(gr2d->reset);

	/*
	 * GR2D module shouldn't be reset while hardware is idling, otherwise
	 * host1x's cmdproc will stuck on trying to access any G2 register
	 * after reset. GR2D module could be either hot-reset or reset after
	 * power-gating of the HEG partition. Hence we will put in reset only
	 * the memory client part of the module, the HEG GENPD will take care
	 * of resetting GR2D module across power-gating.
	 */
	err = reset_control_acquire(gr2d->reset_mc);
	if (err) {
		dev_err(dev, "failed to acquire MC reset: %d\n", err);
		return err;
	}

	err = reset_control_assert(gr2d->reset_mc);
	if (err) {
		dev_err(dev, "failed to assert MC reset: %d\n", err);
		return err;
	}

	clk_disable_unprepare(gr2d->clk);

	/* remove performance vote */
	err = dev_pm_opp_set_rate(dev, 0);
	if (err) {
		dev_err(dev, "failed to set clock rate: %d\n", err);
		return err;
	}

	return 0;
}

static int __maybe_unused gr2d_runtime_resume(struct device *dev)
{
	struct gr2d *gr2d = dev_get_drvdata(dev);
	int err;

	err = dev_pm_opp_set_rate(dev, clk_get_rate(gr2d->clk));
	if (err) {
		dev_err(dev, "failed to set clock rate: %d\n", err);
		return err;
	}

	reset_control_release(gr2d->reset_mc);

	err = reset_control_acquire(gr2d->reset);
	if (err) {
		dev_err(dev, "failed to acquire reset: %d\n", err);
		return err;
	}

	err = clk_prepare_enable(gr2d->clk);
	if (err) {
		dev_err(dev, "failed to enable clock: %d\n", err);
		goto release_reset;
	}

	err = reset_control_deassert(gr2d->reset);
	if (err) {
		dev_err(dev, "failed to deassert reset: %d\n", err);
		return err;
	}

	host1x_channel_reinit(gr2d->channel->channel);
	drm_sched_resubmit_jobs(&gr2d->channel->sched);
	drm_sched_start(&gr2d->channel->sched, false);

	return 0;

release_reset:
	reset_control_release(gr2d->reset);

	return err;
}

static const struct dev_pm_ops tegra_gr2d_pm = {
	SET_RUNTIME_PM_OPS(gr2d_runtime_suspend, gr2d_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
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
