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

struct gr3d_soc {
	unsigned int version;
};

struct gr3d {
	struct iommu_group *group;
	struct reset_control *reset;
	struct tegra_drm_client client;
	struct tegra_drm_channel *channel;
	struct host1x_gather init_gather;

	const struct gr3d_soc *soc;
	struct clk_bulk_data *clocks;
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
					       128, 3, 0, 600, "3d channel");
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

	err = reset_control_assert(gr3d->reset);
	if (err) {
		dev_err(client->dev, "failed to assert reset: %d\n", err);
		return err;
	}

	usleep_range(10, 20);

	err = reset_control_deassert(gr3d->reset);
	if (err) {
		dev_err(client->dev, "failed to deassert reset: %d\n", err);
		return err;
	}

	return 0;
}

static void gr3d_pm_runtime_release(void *dev)
{
	pm_runtime_dont_use_autosuspend(dev);
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

	err = devm_add_action_or_reset(dev, (void *)device_link_del, link);
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
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 200);

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

static struct reset_control *devm_gr3d_get_reset(struct device *dev)
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

static int gr3d_probe(struct platform_device *pdev)
{
	struct tegra_core_opp_params opp_params = {};
	struct opp_table *opp_table;
	struct gr3d *gr3d;
	unsigned int i;
	int err;

	gr3d = devm_kzalloc(&pdev->dev, sizeof(*gr3d), GFP_KERNEL);
	if (!gr3d)
		return -ENOMEM;

	platform_set_drvdata(pdev, gr3d);

	gr3d->soc = of_device_get_match_data(&pdev->dev);

	err = devm_clk_bulk_get_all(&pdev->dev, &gr3d->clocks);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to get clocks: %d\n", err);
		return err;
	}
	gr3d->nclocks = err;

	gr3d->reset = devm_gr3d_get_reset(&pdev->dev);
	if (IS_ERR(gr3d->reset)) {
		dev_err(&pdev->dev, "failed to get reset: %pe\n", gr3d->reset);
		return PTR_ERR(gr3d->reset);
	}

	err = devm_gr3d_init_power(&pdev->dev, gr3d);
	if (err)
		return err;

	opp_table = devm_pm_opp_register_set_opp_helper(&pdev->dev, gr3d_set_opp);
	if (IS_ERR(opp_table))
		return PTR_ERR(opp_table);

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
	gr3d->client.unprepare_job = gr3d_unprepare_job;
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

static int gr3d_legacy_domain_power_up(struct device *dev, const char *name,
				       unsigned int id)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	struct reset_control *reset;
	struct clk *clk;
	unsigned int i;
	int err;

	for (i = 0; i < gr3d->nclocks; i++) {
		if (!strcmp(gr3d->clocks[i].id, name)) {
			clk = gr3d->clocks[i].clk;
			break;
		}
	}

	if (i == gr3d->nclocks)
		return 0;

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
	 * tegra_powergate_sequence_power_up() leaves clocks enabled
	 * while GENPD not, hence keep clock-enable balanced.
	 */
	clk_disable_unprepare(clk);

	return 0;
}

static int gr3d_legacy_power_up(struct device *dev)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	int err;

	if (gr3d->legacy_pd) {
		err = gr3d_legacy_domain_power_up(dev, "3d",
						  TEGRA_POWERGATE_3D);
		if (err)
			return err;

		err = gr3d_legacy_domain_power_up(dev, "3d2",
						  TEGRA_POWERGATE_3D1);
		if (err)
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

	err = reset_control_assert(gr3d->reset);
	if (err) {
		dev_err(dev, "failed to assert reset: %d\n", err);
		return err;
	}

	usleep_range(10, 20);

	/*
	 * Older device-trees don't specify MC resets and power-gating can't
	 * be done safely in that case. Hence we will keep the power ungated
	 * for older DTBs. For newer DTBs, GENPD will perform the power-gating.
	 */

	clk_bulk_disable_unprepare(gr3d->nclocks, gr3d->clocks);
	reset_control_release(gr3d->reset);

	/* remove performance vote */
	err = dev_pm_opp_set_rate(dev, 0);
	if (err) {
		dev_err(dev, "failed to set clock rate: %d\n", err);
		return err;
	}

	return 0;
}

static int __maybe_unused gr3d_runtime_resume(struct device *dev)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);
	int err;

	err = dev_pm_opp_set_rate(dev, clk_get_rate(gr3d->clocks[0].clk));
	if (err) {
		dev_err(dev, "failed to set clock rate: %d\n", err);
		return err;
	}

	err = gr3d_legacy_power_up(dev);
	if (err)
		goto release_reset;

	err = reset_control_acquire(gr3d->reset);
	if (err) {
		dev_err(dev, "failed to acquire reset: %d\n", err);
		return err;
	}

	err = clk_bulk_prepare_enable(gr3d->nclocks, gr3d->clocks);
	if (err) {
		dev_err(dev, "failed to enable clock: %d\n", err);
		goto release_reset;
	}

	err = reset_control_deassert(gr3d->reset);
	if (err) {
		dev_err(dev, "failed to deassert reset: %d\n", err);
		return err;
	}

	host1x_channel_reinit(gr3d->channel->channel);
	drm_sched_resubmit_jobs(&gr3d->channel->sched);
	drm_sched_start(&gr3d->channel->sched, false);

	return 0;

release_reset:
	reset_control_release(gr3d->reset);

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
