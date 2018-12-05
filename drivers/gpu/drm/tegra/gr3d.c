// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Avionic Design GmbH
 * Copyright (C) 2013 NVIDIA Corporation
 */

#include <linux/clk.h>
#include <linux/host1x.h>
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

struct gr3d_soc {
	unsigned int version;
};

struct gr3d {
	struct iommu_group *group;
	struct tegra_drm_client client;
	struct tegra_drm_channel *channel;
	struct clk *clk_secondary;
	struct clk *clk;
	struct reset_control *rst_secondary;
	struct reset_control *rst;
	struct reset_control *rst_mc_secondary;
	struct reset_control *rst_mc;
	struct host1x_gather init_gather;

	const struct gr3d_soc *soc;
	struct clk_bulk_data clocks[2];
	unsigned int nclocks;
	bool legacy_pd;

	DECLARE_BITMAP(addr_regs, GR3D_NUM_REGS);
};

static const struct gr3d_soc tegra20_gr3d_soc = {
	.version = 0x20,
};

static const struct gr3d_soc tegra30_gr3d_soc = {
	.version = 0x30,
};

static const struct gr3d_soc tegra114_gr3d_soc = {
	.version = 0x35,
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

	gr3d->group = tegra_drm_client_iommu_attach(drm_client, false);
	if (IS_ERR(gr3d->group)) {
		err = PTR_ERR(gr3d->group);
		dev_err(client->dev, "failed to attach to domain: %d\n", err);
		return err;
	}

	err = tegra_drm_register_client(tegra_drm, drm_client);
	if (err) {
		dev_err(client->dev, "failed to register client: %d\n", err);
		goto detach_iommu;
	}

	gr3d->channel = tegra_drm_open_channel(tegra_drm, drm_client,
					       /*TEGRA_DRM_PIPE_2D |*/
					       TEGRA_DRM_PIPE_3D,
					       32, 1, 0, 600, "3d channel");
	if (IS_ERR(gr3d->channel)) {
		err = PTR_ERR(gr3d->channel);
		dev_err(client->dev, "failed to open channel: %d\n", err);
		goto unreg_client;
	}

	gr3d->init_gather.bo = host1x_bo_alloc(host, sizeof(gr3d_hw_init),
					       true);
	if (!gr3d->init_gather.bo) {
		err = -ENOMEM;
		dev_err(client->dev, "failed to allocate init bo\n");
		goto close_channel;
	}

	gr3d->init_gather.num_words = ARRAY_SIZE(gr3d_hw_init);

	memcpy(gr3d->init_gather.bo->vaddr, gr3d_hw_init,
	       sizeof(gr3d_hw_init));

	return 0;

close_channel:
	tegra_drm_close_channel(gr3d->channel);

unreg_client:
	tegra_drm_unregister_client(drm_client);

detach_iommu:
	tegra_drm_client_iommu_detach(drm_client, gr3d->group, false);

	return err;
}

static int gr3d_exit(struct host1x_client *client)
{
	struct tegra_drm_client *drm_client = to_tegra_drm_client(client);
	struct drm_device *drm = dev_get_drvdata(client->host);
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct gr3d *gr3d = to_gr3d(drm_client);

	tegra_drm_close_channel(gr3d->channel);
	tegra_drm_unregister_client(drm_client);
	tegra_drm_client_iommu_detach(drm_client, gr3d->group, false);
	host1x_bo_free(host, gr3d->init_gather.bo);

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

	host1x_job_add_init_gather(&job->base, &gr3d->init_gather);

	return 0;
}

static int gr3d_reset_hw(struct tegra_drm_client *drm_client)
{
	struct host1x_client *client = &drm_client->base;
	struct gr3d *gr3d = to_gr3d(drm_client);
	int err;

	/* reset first GPU */
	err = reset_control_assert(gr3d->rst_mc);
	if (err) {
		dev_err(client->dev, "failed to assert mc reset: %d\n", err);
		return err;
	}

	err = reset_control_reset(gr3d->rst);
	if (err) {
		dev_err(client->dev, "failed to reset HW: %d\n", err);
		return err;
	}

	err = reset_control_deassert(gr3d->rst_mc);
	if (err) {
		dev_err(client->dev, "failed to deassert mc reset: %d\n", err);
		return err;
	}

	if (!gr3d->clk_secondary)
		return 0;

	/* reset second GPU */
	err = reset_control_assert(gr3d->rst_mc_secondary);
	if (err) {
		dev_err(client->dev,
			"failed to assert secondary mc reset: %d\n", err);
		return err;
	}

	err = reset_control_reset(gr3d->rst_secondary);
	if (err) {
		dev_err(client->dev,
			"failed to reset secondary HW: %d\n", err);
		return err;
	}

	err = reset_control_deassert(gr3d->rst_mc_secondary);
	if (err) {
		dev_err(client->dev,
			"failed to deassert secondary mc reset: %d\n", err);
		return err;
	}

	return 0;
}

static void gr3d_pm_runtime_release(void *dev)
{
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
}

static int gr3d_link_power_domain(struct device *dev, struct device *pd_dev)
{
	const u32 link_flags = DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME;
	struct device_link *link;
	int err;

	link = device_link_add(dev, pd_dev, link_flags);
	if (!link) {
		dev_err(dev, "failed to link to %s\n", dev_name(pd_dev));
		return -EINVAL;
	}

	err = devm_add_action_or_reset(dev, (void*)device_link_del, link);
	if (err)
		return err;

	return 0;
}

static int devm_gr3d_init_power(struct device *dev, struct gr3d *gr3d)
{
	const char *opp_genpd_names[] = { "3d0", "3d1", NULL };
	struct device **opp_virt_dev;
	struct opp_table *opp_table;
	unsigned int i, num_domains;
	struct device *pd_dev;
	int err;

	err = of_count_phandle_with_args(dev->of_node, "power-domains",
					 "#power-domain-cells");
	if (err < 0) {
		if (err != -ENOENT)
			return err;

		/*
		 * Older device-trees don't use GENPD. In this case we should
		 * toggle power domain manually.
		 */
		gr3d->legacy_pd = true;
		goto power_up;
	}

	num_domains = err;

	/*
	 * The PM domain core automatically attaches a single power domain,
	 * otherwise it skips attaching completely. We have a single domain
	 * on Tegra20 and two domains on Tegra30+.
	 */
	if (dev->pm_domain)
		goto power_up;

	opp_table = devm_pm_opp_attach_genpd(dev, opp_genpd_names, &opp_virt_dev);
	if (IS_ERR(opp_table))
		return PTR_ERR(opp_table);

	for (i = 0; opp_genpd_names[i]; i++) {
		pd_dev = opp_virt_dev[i];
		if (!pd_dev) {
			dev_err(dev, "failed to get %s power domain\n",
				opp_genpd_names[i]);
			return -EINVAL;
		}

		err = gr3d_link_power_domain(dev, pd_dev);
		if (err)
			return err;
	}

power_up:
	pm_runtime_enable(dev);
	err = pm_runtime_get_sync(dev);
	if (err < 0) {
		gr3d_pm_runtime_release(dev);
		return err;
	}

	err = devm_add_action_or_reset(dev, gr3d_pm_runtime_release, dev);
	if (err)
		return err;

	return 0;
}

static int gr3d_set_opp(struct dev_pm_set_opp_data *data)
{
	struct gr3d *gr3d = dev_get_drvdata(data->dev);
	unsigned int i;
	int err;

	for (i = 0; i < gr3d->nclocks; i++) {
		err = clk_set_rate(gr3d->clocks[i].clk, data->new_opp.rate);
		if (err) {
			dev_err(data->dev, "failed to set %s rate to %lu: %d\n",
				gr3d->clocks[i].id, data->new_opp.rate, err);
			return err;
		}
	}

	return 0;
}

static int gr3d_probe(struct platform_device *pdev)
{
	struct tegra_core_opp_params opp_params = {};
	struct device_node *np = pdev->dev.of_node;
	struct opp_table *opp_table;
	struct gr3d *gr3d;
	unsigned int i;
	int err;

	gr3d = devm_kzalloc(&pdev->dev, sizeof(*gr3d), GFP_KERNEL);
	if (!gr3d)
		return -ENOMEM;

	platform_set_drvdata(pdev, gr3d);

	gr3d->soc = of_device_get_match_data(&pdev->dev);

	gr3d->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(gr3d->clk)) {
		dev_err(&pdev->dev, "cannot get clock\n");
		return PTR_ERR(gr3d->clk);
	}

	gr3d->clocks[gr3d->nclocks].id = "3d";
	gr3d->clocks[gr3d->nclocks].clk = gr3d->clk;
	gr3d->nclocks++;

	gr3d->rst = devm_reset_control_get_exclusive_released(&pdev->dev, "3d");
	if (IS_ERR(gr3d->rst)) {
		dev_err(&pdev->dev, "cannot get reset\n");
		return PTR_ERR(gr3d->rst);
	}

	gr3d->rst_mc = devm_reset_control_get_optional(&pdev->dev, "mc");
	if (IS_ERR(gr3d->rst_mc)) {
		dev_err(&pdev->dev, "cannot get MC reset\n");
		return PTR_ERR(gr3d->rst_mc);
	}

	if (of_device_is_compatible(np, "nvidia,tegra30-gr3d")) {
		gr3d->clk_secondary = devm_clk_get(&pdev->dev, "3d2");
		if (IS_ERR(gr3d->clk_secondary)) {
			dev_err(&pdev->dev, "cannot get secondary clock\n");
			return PTR_ERR(gr3d->clk_secondary);
		}

		gr3d->clocks[gr3d->nclocks].id = "3d2";
		gr3d->clocks[gr3d->nclocks].clk = gr3d->clk_secondary;
		gr3d->nclocks++;

		gr3d->rst_secondary =
			devm_reset_control_get_exclusive_released(&pdev->dev, "3d2");
		if (IS_ERR(gr3d->rst_secondary)) {
			dev_err(&pdev->dev, "cannot get secondary reset\n");
			return PTR_ERR(gr3d->rst_secondary);
		}

		gr3d->rst_mc_secondary = devm_reset_control_get_optional(
							&pdev->dev, "mc2");
		if (IS_ERR(gr3d->rst_mc_secondary)) {
			dev_err(&pdev->dev, "cannot get secondary MC reset\n");
			return PTR_ERR(gr3d->rst_mc_secondary);
		}
	}

	err = devm_gr3d_init_power(&pdev->dev, gr3d);
	if (err)
		return err;

	opp_table = devm_pm_opp_register_set_opp_helper(&pdev->dev, gr3d_set_opp);
	if (IS_ERR(opp_table))
		return PTR_ERR(opp_table);

	opp_params.init_state = true;

	err = devm_tegra_core_dev_init_opp_table(&pdev->dev, &opp_params);
	if (err && err != -ENODEV)
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
	gr3d->client.reset_hw = gr3d_reset_hw;
	gr3d->client.addr_regs = gr3d->addr_regs;
	gr3d->client.num_regs = GR3D_NUM_REGS;
	gr3d->client.pipe = TEGRA_DRM_PIPE_3D;

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

	if (gr3d->legacy_pd && gr3d->clk_secondary) {
		err = reset_control_assert(gr3d->rst_secondary);
		if (err) {
			dev_err(dev, "failed to assert secondary reset: %d\n", err);
			return err;
		}

		tegra_powergate_power_off(TEGRA_POWERGATE_3D1);
	}

	if (gr3d->legacy_pd) {
		err = reset_control_assert(gr3d->rst);
		if (err) {
			dev_err(dev, "failed to assert reset: %d\n", err);
			return err;
		}

		tegra_powergate_power_off(TEGRA_POWERGATE_3D);
	}

	clk_bulk_disable_unprepare(gr3d->nclocks, gr3d->clocks);
	reset_control_release(gr3d->rst_secondary);
	reset_control_release(gr3d->rst);

	return 0;
}

static int __maybe_unused gr3d_runtime_resume(struct device *dev)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	int err;

	err = reset_control_acquire(gr3d->rst);
	if (err) {
		dev_err(dev, "failed to acquire reset: %d\n", err);
		return err;
	}

	err = reset_control_acquire(gr3d->rst_secondary);
	if (err) {
		dev_err(dev, "failed to acquire secondary reset: %d\n", err);
		goto release_reset_primary;
	}

	if (gr3d->legacy_pd) {
		err = tegra_powergate_sequence_power_up(TEGRA_POWERGATE_3D,
							gr3d->clk, gr3d->rst);
		if (err)
			goto release_reset_secondary;
	}

	if (gr3d->legacy_pd && gr3d->clk_secondary) {
		err = tegra_powergate_sequence_power_up(TEGRA_POWERGATE_3D1,
							gr3d->clk_secondary,
							gr3d->rst_secondary);
		if (err)
			goto release_reset_secondary;
	}

	err = clk_bulk_prepare_enable(gr3d->nclocks, gr3d->clocks);
	if (err) {
		dev_err(dev, "failed to enable clock: %d\n", err);
		goto release_reset_secondary;
	}

	return 0;

release_reset_secondary:
	reset_control_release(gr3d->rst_secondary);

release_reset_primary:
	reset_control_release(gr3d->rst);

	return err;
}

static __maybe_unused int gr3d_suspend(struct device *dev)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	int err;

	drm_sched_stop(&gr3d->channel->sched, NULL);

	err = pm_runtime_force_suspend(dev);
	if (err < 0)
		return err;

	return 0;
}

static __maybe_unused int gr3d_resume(struct device *dev)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	int err;

	err = pm_runtime_force_resume(dev);
	if (err < 0)
		return err;

	host1x_channel_reinit(gr3d->channel->channel);
	drm_sched_resubmit_jobs(&gr3d->channel->sched);
	drm_sched_start(&gr3d->channel->sched, false);

	return 0;
}

static const struct dev_pm_ops tegra_gr3d_pm = {
	SET_RUNTIME_PM_OPS(gr3d_runtime_suspend, gr3d_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(gr3d_suspend, gr3d_resume)
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
