/*
 * Copyright (C) 2013 Avionic Design GmbH
 * Copyright (C) 2013 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/host1x.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include <soc/tegra/pmc.h>

#include "drm.h"
#include "gem.h"
#include "gr3d.h"

#define OPCODE_SETCL(classid)				\
	((0x0 << 28) | (classid << 6))

#define OPCODE_NONINCR(offset, count)			\
	((0x2 << 28) | (offset << 16) | count)

#define OPCODE_IMM(offset, data)			\
	((0x4 << 28) | ((offset) << 16) | data)

#define OPCODE_GATHER(offset, insert, incr, count)	\
	((0x6 << 28) | (offset << 16) | (insert << 15) | (incr << 14) | count)

#define OPCODE_EXTEND(subop, value)			\
	((0xe << 28) | (subop << 24) | value)

#define ACQUIRE_MLOCK(mlock)	OPCODE_EXTEND(0, mlock)
#define RELEASE_MLOCK(mlock)	OPCODE_EXTEND(1, mlock)
#define OPCODE_NOP		OPCODE_NONINCR(0, 0)

#define INDREAD(modid, offset, autoinc)			\
	((autoinc << 27) | (modid << 18) | ((offset) << 2) | 1)

#define GR3D_REGS(offset, count, incr)			\
{							\
	offset, count, incr,				\
}

struct gr3d_regs_desc {
	unsigned int offset;
	unsigned int count;
	bool incr;
};

struct gr3d {
	struct tegra_drm_client client;
	struct host1x_channel *channel;
	struct clk *clk_secondary;
	struct clk *clk;
	struct reset_control *rst_secondary;
	struct reset_control *rst;

	DECLARE_BITMAP(addr_regs, GR3D_NUM_REGS);
};

/*
 * TODO: Add Tegra30/114 registers.
 */
static const struct gr3d_regs_desc gr3d_regs[] = {
	GR3D_REGS(0x00c,   10, true),
	GR3D_REGS(0x100,   35, true),
	GR3D_REGS(0x124,    3, true),
	GR3D_REGS(0x200,    5, true),
	GR3D_REGS(0x209,    9, true),
	GR3D_REGS(0x300,  102, true),
	GR3D_REGS(0x400,   18, true),
	GR3D_REGS(0x500,    4, true),
	GR3D_REGS(0x520,   32, true),
	GR3D_REGS(0x608,    4, true),
	GR3D_REGS(0x710,   50, true),
	GR3D_REGS(0x820,   32, true),
	GR3D_REGS(0x902,    2, true),
	GR3D_REGS(0xa00,   13, true),
	GR3D_REGS(0xe00,   43, true),
	GR3D_REGS(0x206, 1024, false),
	GR3D_REGS(0x208, 1024, false),
	GR3D_REGS(0x541,   64, false),
	GR3D_REGS(0x601,   64, false),
	GR3D_REGS(0x604,  128, false),
	GR3D_REGS(0x701,   64, false),
	GR3D_REGS(0x801,   64, false),
	GR3D_REGS(0x804,  512, false),
	GR3D_REGS(0x806,   64, false),
	GR3D_REGS(0x901,   64, false),
};

static inline struct gr3d *to_gr3d(struct tegra_drm_client *client)
{
	return container_of(client, struct gr3d, client);
}

static int gr3d_init(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct drm_device *dev = dev_get_drvdata(client->parent);
	unsigned long flags = HOST1X_SYNCPT_HAS_BASE;
	struct gr3d *gr3d = to_gr3d(drm);

	gr3d->channel = host1x_channel_request(client->dev);
	if (!gr3d->channel)
		return -ENOMEM;

	client->syncpts[0] = host1x_syncpt_request(client->dev, flags);
	if (!client->syncpts[0]) {
		host1x_channel_put(gr3d->channel);
		return -ENOMEM;
	}

	return tegra_drm_register_client(dev->dev_private, drm);
}

static int gr3d_exit(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct drm_device *dev = dev_get_drvdata(client->parent);
	struct gr3d *gr3d = to_gr3d(drm);
	int err;

	err = tegra_drm_unregister_client(dev->dev_private, drm);
	if (err < 0)
		return err;

	host1x_syncpt_free(client->syncpts[0]);
	host1x_channel_put(gr3d->channel);

	return 0;
}

static int gr3d_reset(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct gr3d *gr3d = to_gr3d(drm);
	int err;

	err = reset_control_assert(gr3d->rst);
	if (err) {
		dev_err(client->dev, "Failed to assert reset: %d\n", err);
		return err;
	}

	err = reset_control_assert(gr3d->rst_secondary);
	if (err) {
		dev_err(client->dev, "Failed to assert secondary reset: %d\n",
			err);
		return err;
	}

	usleep_range(1000, 2000);

	err = reset_control_deassert(gr3d->rst_secondary);
	if (err) {
		dev_err(client->dev, "Failed to deassert secondary reset: %d\n",
			err);
		return err;
	}

	err = reset_control_deassert(gr3d->rst);
	if (err) {
		dev_err(client->dev, "Failed to deassert reset: %d\n", err);
		return err;
	}

	return 0;
}

static const struct host1x_client_ops gr3d_client_ops = {
	.init = gr3d_init,
	.exit = gr3d_exit,
	.reset = gr3d_reset,
};

static int gr3d_allocate_ctx(struct host1x_client *client,
			     struct host1x_bo **bo)
{
	struct drm_device *dev = dev_get_drvdata(client->parent);
	struct tegra_drm *tegra = dev->dev_private;
	struct tegra_bo *obj;

	obj = tegra_bo_create(tegra->drm, SZ_16K, 0);
	if (!obj)
		return -ENOMEM;

	*bo =  &obj->base;

	return 0;
}

static int gr3d_initialize_ctx(struct host1x_client *client,
			       u32 class,
			       u32 *bo_vaddr,
			       dma_addr_t bo_dma,
			       u32 *bo_offset,
			       unsigned int *words_num,
			       struct host1x_context_push_data **restore_data,
			       struct host1x_context_push_data **store_data,
			       unsigned int *restore_pushes,
			       unsigned int *store_pushes)
{
	struct host1x_context_push_data *restore;
	struct host1x_context_push_data *store;
	const struct gr3d_regs_desc *regs;
	unsigned int pushes_num;
	unsigned int ind_regs;
	unsigned int offset;
	unsigned int count;
	unsigned int words;
	unsigned int i;
	bool incr;

	/* count the number of indirect registers */
	for (i = 0, ind_regs = 0; i < ARRAY_SIZE(gr3d_regs); i++)
		if (!gr3d_regs[i].incr)
			ind_regs++;

	pushes_num = ARRAY_SIZE(gr3d_regs) * 2 + ind_regs * 2 + 1;

	store = kmalloc_array(pushes_num, sizeof(*store), GFP_KERNEL);
	if (!store)
		return -ENOMEM;

	*store_data = store;
	*store_pushes = pushes_num;

	pushes_num = ARRAY_SIZE(gr3d_regs) + ind_regs + 1;

	restore = kmalloc_array(pushes_num, sizeof(*restore), GFP_KERNEL);
	if (!restore) {
		kfree(store);
		return -ENOMEM;
	}

	*restore_data = restore;
	*restore_pushes = pushes_num;

	store->word0 = OPCODE_SETCL(HOST1X_CLASS_HOST1X);
	store->word1 = OPCODE_NOP;
	store++;

	restore->word0 = OPCODE_SETCL(class);
	restore->word1 = OPCODE_NOP;
	restore++;

	for (i = words = 0, regs = gr3d_regs; i < ARRAY_SIZE(gr3d_regs);
	     i++, regs++, words += count) {
		offset = regs->offset;
		count = regs->count;
		incr = regs->incr;

		/*
		 * store: It is important to reset indirect registers offset
		 *        right before reading them, seems it configures the
		 *        IO port. Otherwise a couple of first read words
		 *        could be skipped / clobbered.
		 */
		if (!incr) {
			store->word0 = OPCODE_SETCL(class);
			store->word1 = OPCODE_IMM(offset - 1, 0);
			store++;

			store->word0 = OPCODE_SETCL(HOST1X_CLASS_HOST1X);
			store->word1 = OPCODE_NOP;
			store++;
		}

		/* store: setup indirect registers access pointer */
		store->word0 = OPCODE_NONINCR(0x2d, 1);
		store->word1 = INDREAD(HOST1X_MODULE_GR3D, offset, incr);
		store++;

		/* store: indirectly read 3d regs and push them to 'out' FIFO */
		store->word0 = OPCODE_GATHER(0x2e, 1, 0, count);
		store->word1 = bo_dma;
		store++;

		/* restore: reset indirect registers offset */
		if (!incr) {
			restore->word0 = OPCODE_IMM(offset - 1, 0);
			restore->word1 = OPCODE_NOP;
			restore++;
		}

		/* restore: fetch data from BO and write it indirectly to 3d */
		restore->word0 = OPCODE_GATHER(offset, 1, incr, count);
		restore->word1 = bo_dma + words * sizeof(u32);
		restore++;
	}

	*words_num = words;

	return 0;
}

static void gr3d_debug_ctx(struct host1x_client *client, u32 *bo_vaddr)
{
	struct device *dev = client->dev;
	const struct gr3d_regs_desc *regs;
	unsigned int offset;
	unsigned int count;
	unsigned int words;
	unsigned int i, k;
	bool incr;

	if (!(drm_debug & DRM_UT_DRIVER))
		return;

	for (i = 0, words = 0, regs = gr3d_regs; i < ARRAY_SIZE(gr3d_regs);
	     i++, regs++, words += count) {
		offset = regs->offset;
		count = regs->count;
		incr = regs->incr;

		DRM_DEV_DEBUG_DRIVER(dev,
				     "%p[%u] offset %03X count %u incr %d\n",
				     bo_vaddr, words, offset, count, incr);

		for (k = 0; k < count; k++) {
			DRM_DEV_DEBUG_DRIVER(dev, "%p[%u] [%03X] <= %08X\n",
					     bo_vaddr, words + k, offset,
					     bo_vaddr[words + k]);
			if (incr)
				offset++;
		}
	}
}

static const struct host1x_context_ops gr3d_context_ops = {
	.initialize = gr3d_initialize_ctx,
	.allocate = gr3d_allocate_ctx,
	.debug = gr3d_debug_ctx,
};

static int gr3d_open_channel(struct tegra_drm_client *client,
			     struct tegra_drm_context *context,
			     enum drm_tegra_client clientid)
{
	struct device_node *np = client->base.dev->of_node;
	struct gr3d *gr3d = to_gr3d(client);
	struct host1x_client *cl = &client->base;
	struct host1x_syncpt *sp = cl->syncpts[0];

	if (clientid != DRM_TEGRA_CLIENT_GR3D)
		return -ENODEV;

	context->channel = host1x_channel_get(gr3d->channel);
	if (!context->channel)
		return -ENOMEM;

	/*
	 * Yet, context switching is implemented only for Tegra20,
	 * this check should be removed once Tegra30+ would gain
	 * context switching support.
	 */
	if (of_device_is_compatible(np, "nvidia,tegra30-gr3d") ||
	    of_device_is_compatible(np, "nvidia,tegra114-gr3d"))
		return 0;

	context->hwctx = host1x_create_context(&gr3d_context_ops,
					       context->channel, cl, sp,
					       HOST1X_CLASS_GR3D, true,
					       false, true);
	if (IS_ERR(context->hwctx)) {
		host1x_channel_put(context->channel);
		return PTR_ERR(context->hwctx);
	}

	return 0;
}

static void gr3d_close_channel(struct tegra_drm_context *context)
{
	host1x_context_put(context->hwctx);
	host1x_channel_put(context->channel);
}

static int gr3d_is_addr_reg(struct device *dev, u32 offset)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);

	if (offset >= GR3D_NUM_REGS)
		return 0;

	if (test_bit(offset, gr3d->addr_regs))
		return 1;

	return 0;
}

static const struct tegra_drm_client_ops gr3d_ops = {
	.open_channel = gr3d_open_channel,
	.close_channel = gr3d_close_channel,
	.is_addr_reg = gr3d_is_addr_reg,
	.submit = tegra_drm_submit,
};

static const struct of_device_id tegra_gr3d_match[] = {
	{ .compatible = "nvidia,tegra114-gr3d" },
	{ .compatible = "nvidia,tegra30-gr3d" },
	{ .compatible = "nvidia,tegra20-gr3d" },
	{ }
};
MODULE_DEVICE_TABLE(of, tegra_gr3d_match);

static const u32 gr3d_addr_regs[] = {
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

static int gr3d_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct host1x_syncpt **syncpts;
	struct gr3d *gr3d;
	unsigned int i;
	int err;

	gr3d = devm_kzalloc(&pdev->dev, sizeof(*gr3d), GFP_KERNEL);
	if (!gr3d)
		return -ENOMEM;

	syncpts = devm_kzalloc(&pdev->dev, sizeof(*syncpts), GFP_KERNEL);
	if (!syncpts)
		return -ENOMEM;

	gr3d->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(gr3d->clk)) {
		dev_err(&pdev->dev, "cannot get clock\n");
		return PTR_ERR(gr3d->clk);
	}

	gr3d->rst = devm_reset_control_get(&pdev->dev, "3d");
	if (IS_ERR(gr3d->rst)) {
		dev_err(&pdev->dev, "cannot get reset\n");
		return PTR_ERR(gr3d->rst);
	}

	if (of_device_is_compatible(np, "nvidia,tegra30-gr3d")) {
		gr3d->clk_secondary = devm_clk_get(&pdev->dev, "3d2");
		if (IS_ERR(gr3d->clk_secondary)) {
			dev_err(&pdev->dev, "cannot get secondary clock\n");
			return PTR_ERR(gr3d->clk_secondary);
		}

		gr3d->rst_secondary = devm_reset_control_get(&pdev->dev,
								"3d2");
		if (IS_ERR(gr3d->rst_secondary)) {
			dev_err(&pdev->dev, "cannot get secondary reset\n");
			return PTR_ERR(gr3d->rst_secondary);
		}
	}

	err = tegra_powergate_sequence_power_up(TEGRA_POWERGATE_3D, gr3d->clk,
						gr3d->rst);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to power up 3D unit\n");
		return err;
	}

	if (gr3d->clk_secondary) {
		err = tegra_powergate_sequence_power_up(TEGRA_POWERGATE_3D1,
							gr3d->clk_secondary,
							gr3d->rst_secondary);
		if (err < 0) {
			dev_err(&pdev->dev,
				"failed to power up secondary 3D unit\n");
			return err;
		}
	}

	INIT_LIST_HEAD(&gr3d->client.base.list);
	gr3d->client.base.ops = &gr3d_client_ops;
	gr3d->client.base.dev = &pdev->dev;
	gr3d->client.base.class = HOST1X_CLASS_GR3D;
	gr3d->client.base.module = HOST1X_MODULE_GR3D;
	gr3d->client.base.syncpts = syncpts;
	gr3d->client.base.num_syncpts = 1;

	INIT_LIST_HEAD(&gr3d->client.list);
	gr3d->client.ops = &gr3d_ops;

	err = host1x_client_register(&gr3d->client.base);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register host1x client: %d\n",
			err);
		return err;
	}

	/* initialize address register map */
	for (i = 0; i < ARRAY_SIZE(gr3d_addr_regs); i++)
		set_bit(gr3d_addr_regs[i], gr3d->addr_regs);

	platform_set_drvdata(pdev, gr3d);

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

	if (gr3d->clk_secondary) {
		tegra_powergate_power_off(TEGRA_POWERGATE_3D1);
		clk_disable_unprepare(gr3d->clk_secondary);
	}

	tegra_powergate_power_off(TEGRA_POWERGATE_3D);
	clk_disable_unprepare(gr3d->clk);

	return 0;
}

struct platform_driver tegra_gr3d_driver = {
	.driver = {
		.name = "tegra-gr3d",
		.of_match_table = tegra_gr3d_match,
	},
	.probe = gr3d_probe,
	.remove = gr3d_remove,
};
