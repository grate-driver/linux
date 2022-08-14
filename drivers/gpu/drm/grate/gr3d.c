// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Avionic Design GmbH
 * Copyright (C) 2013 NVIDIA Corporation
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/host1x-grate.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <drm/gpu_scheduler.h>

#include <soc/tegra/common.h>
#include <soc/tegra/pmc.h>

#include "drm.h"
#include "job.h"
#include "gr3d.h"

#define OPCODE_SETCL(classid, offset, mask)		\
	((0 << 28) | (offset << 16) | (classid << 6) | mask)

#define OPCODE_INCR(offset, count)			\
	((1 << 28) | (offset << 16) | count)

#define RESET_ADDR	TEGRA_POISON_ADDR

enum {
	RST_MC,
	RST_GR3D,
	RST_MC2,
	RST_GR3D2,
	RST_GR3D_MAX,
};

struct gr3d_soc {
	unsigned int version;
	unsigned int num_clocks;
	unsigned int num_resets;
};

struct gr3d {
	struct iommu_group *group;
	struct tegra_drm_client client;
	struct tegra_drm_channel *channel;
	struct host1x_gather init_gather;

	const struct gr3d_soc *soc;
	struct clk_bulk_data *clocks;
	unsigned int nclocks;
	struct reset_control_bulk_data resets[RST_GR3D_MAX];
	unsigned int nresets;

	DECLARE_BITMAP(addr_regs, GR3D_NUM_REGS);
};

static const struct gr3d_soc tegra20_gr3d_soc = {
	.version = 0x20,
	.num_clocks = 1,
	.num_resets = 2,
};

static const struct gr3d_soc tegra30_gr3d_soc = {
	.version = 0x30,
	.num_clocks = 2,
	.num_resets = 4,
};

static const struct gr3d_soc tegra114_gr3d_soc = {
	.version = 0x35,
	.num_clocks = 1,
	.num_resets = 2,
};

static const struct of_device_id tegra_gr3d_match[] = {
	{ .compatible = "nvidia,tegra114-gr3d", .data = &tegra114_gr3d_soc },
	{ .compatible = "nvidia,tegra30-gr3d", .data = &tegra30_gr3d_soc },
	{ .compatible = "nvidia,tegra20-gr3d", .data = &tegra20_gr3d_soc },
	{ }
};
MODULE_DEVICE_TABLE(of, tegra_gr3d_match);

static const u32 gr3d_hw_init[] = {
	OPCODE_SETCL(HOST1X_CLASS_GR3D, GR3D_QR_ZTAG_ADDR, 0x15),
	RESET_ADDR, RESET_ADDR, RESET_ADDR,
	OPCODE_INCR(GR3D_DW_MEMORY_OUTPUT_ADDRESS, 1), RESET_ADDR,
	OPCODE_INCR(GR3D_GLOBAL_SPILLSURFADDR, 1), RESET_ADDR,
	OPCODE_INCR(GR3D_GLOBAL_SURFADDR(0), 16),
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	OPCODE_INCR(GR3D_GLOBAL_SURFOVERADDR(0), 16),
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	OPCODE_INCR(GR3D_GLOBAL_SAMP01SURFADDR(0), 32),
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
	RESET_ADDR, RESET_ADDR, RESET_ADDR, RESET_ADDR,
};

static inline struct gr3d *to_gr3d(struct tegra_drm_client *client)
{
	return container_of(client, struct gr3d, client);
}

static int gr3d_init(struct host1x_client *client)
{
	struct tegra_drm_client *drm_client = to_tegra_drm_client(client);
	struct drm_device *drm = dev_get_drvdata(client->host);
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct tegra_drm *tegra_drm = drm->dev_private;
	struct gr3d *gr3d = to_gr3d(drm_client);
	int err;

	gr3d->init_gather.bo = host1x_bo_alloc(host, sizeof(gr3d_hw_init),
					       true);
	if (!gr3d->init_gather.bo) {
		dev_err(client->dev, "failed to allocate init bo\n");
		return -ENOMEM;
	}

	gr3d->init_gather.num_words = ARRAY_SIZE(gr3d_hw_init);

	memcpy(gr3d->init_gather.bo->vaddr, gr3d_hw_init,
	       sizeof(gr3d_hw_init));

	gr3d->group = tegra_drm_client_iommu_attach(drm_client, false);
	if (IS_ERR(gr3d->group)) {
		err = PTR_ERR(gr3d->group);
		dev_err(client->dev, "failed to attach to domain: %d\n", err);
		goto bo_free;
	}

	gr3d->channel = tegra_drm_open_channel(tegra_drm, drm_client,
					       /*TEGRA_DRM_PIPE_2D |*/
					       TEGRA_DRM_PIPE_3D,
					       128, 3, 0, 600, "3d channel");
	if (IS_ERR(gr3d->channel)) {
		err = PTR_ERR(gr3d->channel);
		dev_err(client->dev, "failed to open channel: %d\n", err);
		goto detach_iommu;
	}

	pm_runtime_enable(client->dev);
	pm_runtime_use_autosuspend(client->dev);
	pm_runtime_set_autosuspend_delay(client->dev, 200);

	err = tegra_drm_register_client(tegra_drm, drm_client);
	if (err) {
		dev_err(client->dev, "failed to register client: %d\n", err);
		goto disable_rpm;
	}

	return 0;

disable_rpm:
	pm_runtime_dont_use_autosuspend(client->dev);
	pm_runtime_force_suspend(client->dev);

	tegra_drm_close_channel(gr3d->channel);
detach_iommu:
	tegra_drm_client_iommu_detach(drm_client, gr3d->group, false);
bo_free:
	host1x_bo_free(host, gr3d->init_gather.bo);

	return err;
}

static int gr3d_exit(struct host1x_client *client)
{
	struct tegra_drm_client *drm_client = to_tegra_drm_client(client);
	struct drm_device *drm = dev_get_drvdata(client->host);
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct gr3d *gr3d = to_gr3d(drm_client);

	tegra_drm_unregister_client(drm_client);

	pm_runtime_dont_use_autosuspend(client->dev);
	pm_runtime_force_suspend(client->dev);

	tegra_drm_close_channel(gr3d->channel);
	tegra_drm_client_iommu_detach(drm_client, gr3d->group, false);
	host1x_bo_free(host, gr3d->init_gather.bo);

	gr3d->channel = NULL;

	return 0;
}

static const struct host1x_client_ops gr3d_host1x_client_ops = {
	.init = gr3d_init,
	.exit = gr3d_exit,
};

static const u16 gr3d_addr_regs[] = {
	GR3D_IDX_ATTRIBUTE( 0),
	GR3D_IDX_ATTRIBUTE( 1),
	GR3D_IDX_ATTRIBUTE( 2),
	GR3D_IDX_ATTRIBUTE( 3),
	GR3D_IDX_ATTRIBUTE( 4),
	GR3D_IDX_ATTRIBUTE( 5),
	GR3D_IDX_ATTRIBUTE( 6),
	GR3D_IDX_ATTRIBUTE( 7),
	GR3D_IDX_ATTRIBUTE( 8),
	GR3D_IDX_ATTRIBUTE( 9),
	GR3D_IDX_ATTRIBUTE(10),
	GR3D_IDX_ATTRIBUTE(11),
	GR3D_IDX_ATTRIBUTE(12),
	GR3D_IDX_ATTRIBUTE(13),
	GR3D_IDX_ATTRIBUTE(14),
	GR3D_IDX_ATTRIBUTE(15),
	GR3D_IDX_INDEX_BASE,
	GR3D_QR_ZTAG_ADDR,
	GR3D_QR_CTAG_ADDR,
	GR3D_QR_CZ_ADDR,
	GR3D_TEX_TEX_ADDR( 0),
	GR3D_TEX_TEX_ADDR( 1),
	GR3D_TEX_TEX_ADDR( 2),
	GR3D_TEX_TEX_ADDR( 3),
	GR3D_TEX_TEX_ADDR( 4),
	GR3D_TEX_TEX_ADDR( 5),
	GR3D_TEX_TEX_ADDR( 6),
	GR3D_TEX_TEX_ADDR( 7),
	GR3D_TEX_TEX_ADDR( 8),
	GR3D_TEX_TEX_ADDR( 9),
	GR3D_TEX_TEX_ADDR(10),
	GR3D_TEX_TEX_ADDR(11),
	GR3D_TEX_TEX_ADDR(12),
	GR3D_TEX_TEX_ADDR(13),
	GR3D_TEX_TEX_ADDR(14),
	GR3D_TEX_TEX_ADDR(15),
	GR3D_DW_MEMORY_OUTPUT_ADDRESS,
	GR3D_GLOBAL_SURFADDR( 0),
	GR3D_GLOBAL_SURFADDR( 1),
	GR3D_GLOBAL_SURFADDR( 2),
	GR3D_GLOBAL_SURFADDR( 3),
	GR3D_GLOBAL_SURFADDR( 4),
	GR3D_GLOBAL_SURFADDR( 5),
	GR3D_GLOBAL_SURFADDR( 6),
	GR3D_GLOBAL_SURFADDR( 7),
	GR3D_GLOBAL_SURFADDR( 8),
	GR3D_GLOBAL_SURFADDR( 9),
	GR3D_GLOBAL_SURFADDR(10),
	GR3D_GLOBAL_SURFADDR(11),
	GR3D_GLOBAL_SURFADDR(12),
	GR3D_GLOBAL_SURFADDR(13),
	GR3D_GLOBAL_SURFADDR(14),
	GR3D_GLOBAL_SURFADDR(15),
	GR3D_GLOBAL_SPILLSURFADDR,
	GR3D_GLOBAL_SURFOVERADDR( 0),
	GR3D_GLOBAL_SURFOVERADDR( 1),
	GR3D_GLOBAL_SURFOVERADDR( 2),
	GR3D_GLOBAL_SURFOVERADDR( 3),
	GR3D_GLOBAL_SURFOVERADDR( 4),
	GR3D_GLOBAL_SURFOVERADDR( 5),
	GR3D_GLOBAL_SURFOVERADDR( 6),
	GR3D_GLOBAL_SURFOVERADDR( 7),
	GR3D_GLOBAL_SURFOVERADDR( 8),
	GR3D_GLOBAL_SURFOVERADDR( 9),
	GR3D_GLOBAL_SURFOVERADDR(10),
	GR3D_GLOBAL_SURFOVERADDR(11),
	GR3D_GLOBAL_SURFOVERADDR(12),
	GR3D_GLOBAL_SURFOVERADDR(13),
	GR3D_GLOBAL_SURFOVERADDR(14),
	GR3D_GLOBAL_SURFOVERADDR(15),
	GR3D_GLOBAL_SAMP01SURFADDR( 0),
	GR3D_GLOBAL_SAMP01SURFADDR( 1),
	GR3D_GLOBAL_SAMP01SURFADDR( 2),
	GR3D_GLOBAL_SAMP01SURFADDR( 3),
	GR3D_GLOBAL_SAMP01SURFADDR( 4),
	GR3D_GLOBAL_SAMP01SURFADDR( 5),
	GR3D_GLOBAL_SAMP01SURFADDR( 6),
	GR3D_GLOBAL_SAMP01SURFADDR( 7),
	GR3D_GLOBAL_SAMP01SURFADDR( 8),
	GR3D_GLOBAL_SAMP01SURFADDR( 9),
	GR3D_GLOBAL_SAMP01SURFADDR(10),
	GR3D_GLOBAL_SAMP01SURFADDR(11),
	GR3D_GLOBAL_SAMP01SURFADDR(12),
	GR3D_GLOBAL_SAMP01SURFADDR(13),
	GR3D_GLOBAL_SAMP01SURFADDR(14),
	GR3D_GLOBAL_SAMP01SURFADDR(15),
	GR3D_GLOBAL_SAMP23SURFADDR( 0),
	GR3D_GLOBAL_SAMP23SURFADDR( 1),
	GR3D_GLOBAL_SAMP23SURFADDR( 2),
	GR3D_GLOBAL_SAMP23SURFADDR( 3),
	GR3D_GLOBAL_SAMP23SURFADDR( 4),
	GR3D_GLOBAL_SAMP23SURFADDR( 5),
	GR3D_GLOBAL_SAMP23SURFADDR( 6),
	GR3D_GLOBAL_SAMP23SURFADDR( 7),
	GR3D_GLOBAL_SAMP23SURFADDR( 8),
	GR3D_GLOBAL_SAMP23SURFADDR( 9),
	GR3D_GLOBAL_SAMP23SURFADDR(10),
	GR3D_GLOBAL_SAMP23SURFADDR(11),
	GR3D_GLOBAL_SAMP23SURFADDR(12),
	GR3D_GLOBAL_SAMP23SURFADDR(13),
	GR3D_GLOBAL_SAMP23SURFADDR(14),
	GR3D_GLOBAL_SAMP23SURFADDR(15),
};

static int
gr3d_refine_class(struct tegra_drm_client *client, u64 pipes,
		  unsigned int *classid)
{
	enum drm_tegra_cmdstream_class drm_class = *classid;

	if (!(pipes & TEGRA_DRM_PIPE_3D))
		return -EINVAL;

	switch (drm_class) {
	case DRM_TEGRA_CMDSTREAM_CLASS_GR3D:
		*classid = HOST1X_CLASS_GR3D;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int
gr3d_prepare_job(struct tegra_drm_client *client, struct tegra_drm_job *job)
{
	struct gr3d *gr3d = to_gr3d(client);
	int err;

	err = pm_runtime_resume_and_get(client->base.dev);
	if (err < 0)
		return err;

	host1x_job_add_init_gather(&job->base, &gr3d->init_gather);

	return 0;
}

static int
gr3d_unprepare_job(struct tegra_drm_client *client, struct tegra_drm_job *job)
{
	pm_runtime_mark_last_busy(client->base.dev);
	pm_runtime_put_autosuspend(client->base.dev);

	return 0;
}

static int gr3d_reset_hw(struct tegra_drm_client *drm_client)
{
	struct host1x_client *client = &drm_client->base;
	struct gr3d *gr3d = to_gr3d(drm_client);
	int err;

	err = reset_control_bulk_assert(gr3d->nresets, gr3d->resets);
	if (err) {
		dev_err(client->dev, "failed to assert reset: %d\n", err);
		return err;
	}

	usleep_range(10, 20);

	err = reset_control_bulk_deassert(gr3d->nresets, gr3d->resets);
	if (err) {
		dev_err(client->dev, "failed to deassert reset: %d\n", err);
		return err;
	}

	return 0;
}

static int gr3d_power_up_legacy_domain(struct device *dev, const char *name,
				       unsigned int id)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	struct reset_control *reset;
	struct clk *clk;
	unsigned int i;
	int err;

	/*
	 * Tegra20 device-tree doesn't specify 3d clock name and there is only
	 * one clock for Tegra20. Tegra30+ device-trees always specified names
	 * for the clocks.
	 */
	if (gr3d->nclocks == 1) {
		if (id == TEGRA_POWERGATE_3D1)
			return 0;

		clk = gr3d->clocks[0].clk;
	} else {
		for (i = 0; i < gr3d->nclocks; i++) {
			if (WARN_ON(!gr3d->clocks[i].id))
				continue;

			if (!strcmp(gr3d->clocks[i].id, name)) {
				clk = gr3d->clocks[i].clk;
				break;
			}
		}

		if (WARN_ON(i == gr3d->nclocks))
			return -EINVAL;
	}

	/*
	 * We use array of resets, which includes MC resets, and MC
	 * reset shouldn't be asserted while hardware is gated because
	 * MC flushing will fail for gated hardware. Hence for legacy
	 * PD we request the individual reset separately.
	 */
	reset = reset_control_get_exclusive_released(dev, name);
	if (IS_ERR(reset))
		return PTR_ERR(reset);

	err = reset_control_acquire(reset);
	if (err) {
		dev_err(dev, "failed to acquire %s reset: %d\n", name, err);
	} else {
		err = tegra_powergate_sequence_power_up(id, clk, reset);
		reset_control_release(reset);
	}

	reset_control_put(reset);
	if (err)
		return err;

	/*
	 * tegra_powergate_sequence_power_up() leaves clocks enabled,
	 * while GENPD not. Hence keep clock-enable balanced.
	 */
	clk_disable_unprepare(clk);

	return 0;
}

static void gr3d_del_link(void *link)
{
	device_link_del(link);
}

static int gr3d_init_power(struct device *dev, struct gr3d *gr3d)
{
	static const char * const opp_genpd_names[] = { "3d0", "3d1", NULL };
	const u32 link_flags = DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME;
	struct device **opp_virt_devs, *pd_dev;
	struct device_link *link;
	unsigned int i;
	int err;
	struct dev_pm_opp_config config = {
		.genpd_names = opp_genpd_names,
		.virt_devs = &opp_virt_devs,
	};

	err = of_count_phandle_with_args(dev->of_node, "power-domains",
					 "#power-domain-cells");
	if (err < 0) {
		if (err != -ENOENT)
			return err;

		/*
		 * Older device-trees don't use GENPD. In this case we should
		 * toggle power domain manually.
		 */
		err = gr3d_power_up_legacy_domain(dev, "3d",
						  TEGRA_POWERGATE_3D);
		if (err)
			return err;

		err = gr3d_power_up_legacy_domain(dev, "3d2",
						  TEGRA_POWERGATE_3D1);
		if (err)
			return err;

		return 0;
	}

	/*
	 * The PM domain core automatically attaches a single power domain,
	 * otherwise it skips attaching completely. We have a single domain
	 * on Tegra20 and two domains on Tegra30+.
	 */
	if (dev->pm_domain)
		return 0;

	err = devm_pm_opp_set_config(dev, &config);
	if (err)
		return err;

	for (i = 0; opp_genpd_names[i]; i++) {
		pd_dev = opp_virt_devs[i];
		if (!pd_dev) {
			dev_err(dev, "failed to get %s power domain\n",
				opp_genpd_names[i]);
			return -EINVAL;
		}

		link = device_link_add(dev, pd_dev, link_flags);
		if (!link) {
			dev_err(dev, "failed to link to %s\n", dev_name(pd_dev));
			return -EINVAL;
		}

		err = devm_add_action_or_reset(dev, gr3d_del_link, link);
		if (err)
			return err;
	}

	return 0;
}

// static int gr3d_set_opp(struct dev_pm_set_opp_data *data)
// {
// 	struct gr3d *gr3d = dev_get_drvdata(data->dev);
// 	unsigned int i;
// 	int err;
//
// 	for (i = 0; i < gr3d->nclocks; i++) {
// 		err = clk_set_rate(gr3d->clocks[i].clk, data->new_opp.rate);
// 		if (err) {
// 			dev_err(data->dev, "failed to set %s rate to %lu: %d\n",
// 				gr3d->clocks[i].id, data->new_opp.rate, err);
// 			goto restore;
// 		}
// 	}
//
// 	return 0;
//
// restore:
// 	while (i--)
// 		clk_set_rate(gr3d->clocks[i].clk, data->old_opp.rate);
//
// 	return err;
// }

static int gr3d_get_clocks(struct device *dev, struct gr3d *gr3d)
{
	int err;

	err = devm_clk_bulk_get_all(dev, &gr3d->clocks);
	if (err < 0) {
		dev_err(dev, "failed to get clock: %d\n", err);
		return err;
	}
	gr3d->nclocks = err;

	if (gr3d->nclocks != gr3d->soc->num_clocks) {
		dev_err(dev, "invalid number of clocks: %u\n", gr3d->nclocks);
		return -ENOENT;
	}

	return 0;
}

static int gr3d_get_resets(struct device *dev, struct gr3d *gr3d)
{
	int err;

	gr3d->resets[RST_MC].id = "mc";
	gr3d->resets[RST_MC2].id = "mc2";
	gr3d->resets[RST_GR3D].id = "3d";
	gr3d->resets[RST_GR3D2].id = "3d2";
	gr3d->nresets = gr3d->soc->num_resets;

	err = devm_reset_control_bulk_get_optional_exclusive_released(
				dev, gr3d->nresets, gr3d->resets);
	if (err) {
		dev_err(dev, "failed to get reset: %d\n", err);
		return err;
	}

	if (WARN_ON(!gr3d->resets[RST_GR3D].rstc) ||
	    WARN_ON(!gr3d->resets[RST_GR3D2].rstc && gr3d->nresets == 4))
		return -ENOENT;

	return 0;
}

static int gr3d_probe(struct platform_device *pdev)
{
	struct gr3d *gr3d;
	unsigned int i;
	int err;

	gr3d = devm_kzalloc(&pdev->dev, sizeof(*gr3d), GFP_KERNEL);
	if (!gr3d)
		return -ENOMEM;

	platform_set_drvdata(pdev, gr3d);

	gr3d->soc = of_device_get_match_data(&pdev->dev);

	err = gr3d_get_clocks(&pdev->dev, gr3d);
	if (err)
		return err;

	err = gr3d_get_resets(&pdev->dev, gr3d);
	if (err)
		return err;

	err = gr3d_init_power(&pdev->dev, gr3d);
	if (err)
		return err;

	INIT_LIST_HEAD(&gr3d->client.base.list);
	gr3d->client.base.ops = &gr3d_host1x_client_ops;
	gr3d->client.base.dev = &pdev->dev;
	gr3d->client.base.class = HOST1X_CLASS_GR3D;

	/* initialize address register map */
	for (i = 0; i < ARRAY_SIZE(gr3d_addr_regs); i++)
		set_bit(gr3d_addr_regs[i], gr3d->addr_regs);

	gr3d->client.refine_class = gr3d_refine_class;
	gr3d->client.prepare_job = gr3d_prepare_job;
	gr3d->client.unprepare_job = gr3d_unprepare_job;
	gr3d->client.reset_hw = gr3d_reset_hw;
	gr3d->client.addr_regs = gr3d->addr_regs;
	gr3d->client.num_regs = GR3D_NUM_REGS;
	gr3d->client.pipe = TEGRA_DRM_PIPE_3D;

// 	err = devm_pm_opp_register_set_opp_helper(&pdev->dev, gr3d_set_opp);
// 	if (err)
// 		return err;

	err = devm_tegra_core_dev_init_opp_table_common(&pdev->dev);
	if (err)
		return err;

	err = host1x_client_register(&gr3d->client.base);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register host1x client: %d\n",
			err);
		return err;
	}

	return 0;
}

static int gr3d_remove(struct platform_device *pdev)
{
	struct gr3d *gr3d = platform_get_drvdata(pdev);
	int err;

	err = host1x_client_unregister(&gr3d->client.base);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to unregister host1x client: %d\n",
			err);
		return err;
	}

	return 0;
}

static int __maybe_unused gr3d_runtime_suspend(struct device *dev)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	int err;

	drm_sched_stop(&gr3d->channel->sched, NULL);
	host1x_channel_stop(gr3d->channel->channel);

	err = reset_control_bulk_assert(gr3d->nresets, gr3d->resets);
	if (err) {
		dev_err(dev, "failed to assert reset: %d\n", err);
		goto resume_host1x;
	}

	usleep_range(10, 20);

	/*
	 * Older device-trees don't specify MC resets and power-gating can't
	 * be done safely in that case. Hence we will keep the power ungated
	 * for older DTBs. For newer DTBs, GENPD will perform the power-gating.
	 */

	clk_bulk_disable_unprepare(gr3d->nclocks, gr3d->clocks);
	reset_control_bulk_release(gr3d->nresets, gr3d->resets);

	return 0;

resume_host1x:
	host1x_channel_reinit(gr3d->channel->channel);
	drm_sched_resubmit_jobs(&gr3d->channel->sched);
	drm_sched_start(&gr3d->channel->sched, false);

	return err;
}

static int __maybe_unused gr3d_runtime_resume(struct device *dev)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	int err;

	err = reset_control_bulk_acquire(gr3d->nresets, gr3d->resets);
	if (err) {
		dev_err(dev, "failed to acquire reset: %d\n", err);
		return err;
	}

	err = clk_bulk_prepare_enable(gr3d->nclocks, gr3d->clocks);
	if (err) {
		dev_err(dev, "failed to enable clock: %d\n", err);
		goto release_reset;
	}

	err = reset_control_bulk_deassert(gr3d->nresets, gr3d->resets);
	if (err) {
		dev_err(dev, "failed to deassert reset: %d\n", err);
		goto disable_clk;
	}

	host1x_channel_reinit(gr3d->channel->channel);
	drm_sched_resubmit_jobs(&gr3d->channel->sched);
	drm_sched_start(&gr3d->channel->sched, false);

	return 0;

disable_clk:
	clk_bulk_disable_unprepare(gr3d->nclocks, gr3d->clocks);
release_reset:
	reset_control_bulk_release(gr3d->nresets, gr3d->resets);

	return err;
}

static const struct dev_pm_ops tegra_gr3d_pm = {
	SET_RUNTIME_PM_OPS(gr3d_runtime_suspend, gr3d_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

struct platform_driver tegra_gr3d_driver = {
	.driver = {
		.name = "tegra-gr3d",
		.of_match_table = tegra_gr3d_match,
		.pm = &tegra_gr3d_pm,
	},
	.probe = gr3d_probe,
	.remove = gr3d_remove,
};
