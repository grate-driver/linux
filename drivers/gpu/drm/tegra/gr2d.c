/*
 * Copyright (c) 2012-2013, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/reset.h>

#include "drm.h"
#include "gem.h"
#include "gr2d.h"

#define GR2D_SW_CTX_G2_CLASS	HOST1X_CLASS_GR2D_G2_CTX5
#define GR2D_SW_CTX_SB_CLASS	HOST1X_CLASS_GR2D_SB_CTX3

#define OPCODE_SETCL(classid)				\
	((0x0 << 28) | (classid << 6))

#define OPCODE_NONINCR(offset, count)			\
	((0x2 << 28) | (offset << 16) | count)

#define OPCODE_IMM(offset, data)			\
	((0x4 << 28) | (offset << 16) | data)

#define OPCODE_GATHER(offset, insert, incr, count)	\
	((0x6 << 28) | (offset << 16) | (insert << 15) | (incr << 14) | count)

#define OPCODE_NOP		OPCODE_NONINCR(0, 0)

#define INDREAD(modid, offset, autoinc)			\
	((autoinc << 27) | (modid << 18) | ((offset) << 2) | 1)

#define GR2D_REGINFO(offset, count)			\
{							\
	offset, count,					\
}

struct gr2d_regs_desc {
	unsigned int offset;
	unsigned int count;
};

struct gr2d {
	struct tegra_drm_client client;
	struct host1x_channel *channel;
	struct reset_control *rst;
	struct clk *clk;

	DECLARE_BITMAP(addr_regs, GR2D_NUM_REGS);

	unsigned long g2_contexts;
	unsigned long sb_contexts;
};

static const struct gr2d_regs_desc gr2d_regs[] = {
	GR2D_REGINFO(0x0c,  1),
	GR2D_REGINFO(0x11,  9),
	GR2D_REGINFO(0x1a,  2),
	GR2D_REGINFO(0x1c, 10),
	GR2D_REGINFO(0x26,  1),
	GR2D_REGINFO(0x27,  4),
	GR2D_REGINFO(0x2b,  3),
	GR2D_REGINFO(0x2e,  3),
	GR2D_REGINFO(0x31,  2),
	GR2D_REGINFO(0x33, 20),
	GR2D_REGINFO(0x47,  6),
	GR2D_REGINFO(0x09,  3),
};

static inline struct gr2d *to_gr2d(struct tegra_drm_client *client)
{
	return container_of(client, struct gr2d, client);
}

static int gr2d_init(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct drm_device *dev = dev_get_drvdata(client->parent);
	unsigned long flags = HOST1X_SYNCPT_HAS_BASE;
	struct gr2d *gr2d = to_gr2d(drm);

	gr2d->channel = host1x_channel_request(client->dev);
	if (!gr2d->channel)
		return -ENOMEM;

	client->syncpts[0] = host1x_syncpt_request(client->dev, flags);
	if (!client->syncpts[0]) {
		host1x_channel_put(gr2d->channel);
		return -ENOMEM;
	}

	return tegra_drm_register_client(dev->dev_private, drm);
}

static int gr2d_exit(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct drm_device *dev = dev_get_drvdata(client->parent);
	struct gr2d *gr2d = to_gr2d(drm);
	int err;

	err = tegra_drm_unregister_client(dev->dev_private, drm);
	if (err < 0)
		return err;

	host1x_syncpt_free(client->syncpts[0]);
	host1x_channel_put(gr2d->channel);

	return 0;
}

static int gr2d_reset(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct gr2d *gr2d = to_gr2d(drm);
	int err;

	err = reset_control_assert(gr2d->rst);
	if (err) {
		dev_err(client->dev, "Failed to assert reset: %d\n", err);
		return err;
	}

	usleep_range(1000, 2000);

	err = reset_control_deassert(gr2d->rst);
	if (err) {
		dev_err(client->dev, "Failed to deassert reset: %d\n", err);
		return err;
	}

	return 0;
}

static const struct host1x_client_ops gr2d_client_ops = {
	.init = gr2d_init,
	.exit = gr2d_exit,
	.reset = gr2d_reset,
};

static int gr2d_allocate_ctx(struct host1x_client *client,
			     struct host1x_bo **bo)
{
	struct drm_device *dev = dev_get_drvdata(client->parent);
	struct tegra_drm *tegra = dev->dev_private;
	struct tegra_bo *obj;

	obj = tegra_bo_create(tegra->drm, SZ_1K, 0);
	if (!obj)
		return -ENOMEM;

	*bo =  &obj->base;

	return 0;
}

static int gr2d_initialize_ctx(struct host1x_client *client,
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
	const struct gr2d_regs_desc *regs;
	unsigned int pushes_num;
	unsigned int offset;
	unsigned int count;
	unsigned int words;
	unsigned int base;
	unsigned int i;

	pushes_num = ARRAY_SIZE(gr2d_regs) * 2 + 1;

	store = kmalloc_array(pushes_num, sizeof(*store), GFP_KERNEL);
	if (!store)
		return -ENOMEM;

	*store_data = store;
	*store_pushes = pushes_num;

	pushes_num = ARRAY_SIZE(gr2d_regs) + 2;

	restore = kmalloc_array(pushes_num, sizeof(*restore), GFP_KERNEL);
	if (!restore) {
		kfree(store);
		return -ENOMEM;
	}

	*restore_data = restore;
	*restore_pushes = pushes_num;

	restore->word0 = OPCODE_SETCL(class);
	restore->word1 = OPCODE_NOP;
	restore++;

	/* first reset trigger registers, they will be restored in the end */
	restore->word0 = OPCODE_GATHER(0, 0, 0, 3);
	restore->word1 = bo_dma;
	restore++;

	bo_vaddr[0] = OPCODE_IMM(0x9, 0);
	bo_vaddr[1] = OPCODE_IMM(0xa, 0);
	bo_vaddr[2] = OPCODE_IMM(0xb, 0);

	store->word0 = OPCODE_SETCL(HOST1X_CLASS_HOST1X);
	store->word1 = OPCODE_NOP;
	store++;

	/* point base to the start of contexts registers bank */
	base = (class & 0xf) * 0x1000;

	for (i = words = 0, regs = gr2d_regs; i < ARRAY_SIZE(gr2d_regs);
	     i++, regs++, words += count) {
		offset = regs->offset;
		count = regs->count;

		/* store: setup indirect registers access pointer */
		store->word0 = OPCODE_NONINCR(0x2d, 1);
		store->word1 = INDREAD(HOST1X_MODULE_GR2D, base + offset, true);
		store++;

		/* store: indirectly read 2d regs and push them to 'out' FIFO */
		store->word0 = OPCODE_GATHER(0x2e, 1, 0, count);
		store->word1 = bo_dma;
		store++;

		/* restore: fetch data from BO and write it indirectly to 2d */
		restore->word0 = OPCODE_GATHER(offset, 1, true, count);
		restore->word1 = bo_dma + (3 + words) * sizeof(u32);
		restore++;
	}

	*words_num = words;
	/* adjust BO address, skipping trigger registers reset */
	*bo_offset = 3 * sizeof(u32);

	return 0;
}

static void gr2d_debug_ctx(struct host1x_client *client, u32 *bo_vaddr)
{
	struct device *dev = client->dev;
	const struct gr2d_regs_desc *regs;
	unsigned int offset;
	unsigned int count;
	unsigned int words;
	unsigned int i, k;
	u32 data;

	if (!(drm_debug & DRM_UT_DRIVER))
		return;

	for (i = 0; i < 3; i++) {
		offset = (bo_vaddr[i] & GENMASK(27, 16)) >> 16;
		data   = (bo_vaddr[i] & GENMASK(15,  0)) >>  0;

		DRM_DEV_DEBUG_DRIVER(dev, "%p[%u] [%03X] <= %08X\n",
				     bo_vaddr, i, offset, data);
	}

	for (i = 0, words = 3, regs = gr2d_regs; i < ARRAY_SIZE(gr2d_regs);
	     i++, regs++, words += count) {
		offset = regs->offset;
		count = regs->count;

		DRM_DEV_DEBUG_DRIVER(dev, "%p[%u] offset %03X count %u\n",
				     bo_vaddr, words, offset, count);

		for (k = 0; k < count; k++, offset++) {
			DRM_DEV_DEBUG_DRIVER(dev, "%p[%u] [%03X] <= %08X\n",
					     bo_vaddr, words + k, offset,
					     bo_vaddr[words + k]);
		}
	}
}

static const struct host1x_context_ops gr2d_context_ops = {
	.initialize = gr2d_initialize_ctx,
	.allocate = gr2d_allocate_ctx,
	.debug = gr2d_debug_ctx,
};

static int gr2d_get_context(struct gr2d *gr2d, enum drm_tegra_client client,
			    u32 *class)
{
	int index;

	/*
	 * There are 5 G2 contexts and 3 SB contexts, we would trade 1 G2
	 * and 1 SB HW context for a software 'switchable' contexts in
	 * order to have unlimited number of contexts.
	 */
	switch (client) {
	case DRM_TEGRA_CLIENT_GR2D_G2:
		index = find_first_zero_bit(&gr2d->g2_contexts, 4);
		index = min(index, 4);

		switch (index) {
		case 0: *class = HOST1X_CLASS_GR2D_G2_CTX1; break;
		case 1: *class = HOST1X_CLASS_GR2D_G2_CTX2; break;
		case 2: *class = HOST1X_CLASS_GR2D_G2_CTX3; break;
		case 3: *class = HOST1X_CLASS_GR2D_G2_CTX4; break;
		default: *class = GR2D_SW_CTX_G2_CLASS; break;
		}

		set_bit(index, &gr2d->g2_contexts);
		break;

	case DRM_TEGRA_CLIENT_GR2D_SB:
		index = find_first_zero_bit(&gr2d->sb_contexts, 2);
		index = min(index, 2);

		switch (index) {
		case 0: *class = HOST1X_CLASS_GR2D_SB_CTX1; break;
		case 1: *class = HOST1X_CLASS_GR2D_SB_CTX2; break;
		default: *class = GR2D_SW_CTX_SB_CLASS; break;
		}

		set_bit(index, &gr2d->sb_contexts);
		break;

	default:
		return -ENODEV;
	}

	return 0;
}

static void gr2d_release_context(struct tegra_drm_context *context)
{
	struct gr2d *gr2d = to_gr2d(context->client);
	struct host1x_client *client = &context->client->base;
	u32 class = host1x_context_class(client, context->hwctx);
	bool g2ctx = false;
	bool sbctx = false;
	int index;

	switch (class) {
	case HOST1X_CLASS_GR2D_G2_CTX1: g2ctx = true; index = 0; break;
	case HOST1X_CLASS_GR2D_G2_CTX2: g2ctx = true; index = 1; break;
	case HOST1X_CLASS_GR2D_G2_CTX3: g2ctx = true; index = 2; break;
	case HOST1X_CLASS_GR2D_G2_CTX4: g2ctx = true; index = 3; break;
	case HOST1X_CLASS_GR2D_G2_CTX5: g2ctx = true; index = 4; break;
	case HOST1X_CLASS_GR2D_SB_CTX1: sbctx = true; index = 0; break;
	case HOST1X_CLASS_GR2D_SB_CTX2: sbctx = true; index = 1; break;
	case HOST1X_CLASS_GR2D_SB_CTX3: sbctx = true; index = 2; break;

	default:
		WARN(1, "Invalid class 0x%X\n", class);
		return;
	}

	if (g2ctx)
		clear_bit(index, &gr2d->g2_contexts);

	if (sbctx)
		clear_bit(index, &gr2d->sb_contexts);
}

static int gr2d_open_channel(struct tegra_drm_client *client,
			     struct tegra_drm_context *context,
			     enum drm_tegra_client clientid)
{
	struct gr2d *gr2d = to_gr2d(client);
	struct host1x_client *cl = &client->base;
	struct host1x_syncpt *sp = cl->syncpts[0];
	bool sw_ctx = false;
	u32 class;
	int err;

	err = gr2d_get_context(gr2d, clientid, &class);
	if (err < 0)
		return err;

	if (class == GR2D_SW_CTX_G2_CLASS ||
	    class == GR2D_SW_CTX_SB_CLASS)
		sw_ctx = true;

	context->channel = host1x_channel_get(gr2d->channel);
	if (!context->channel)
		return -ENOMEM;

	context->hwctx = host1x_create_context(&gr2d_context_ops,
					       context->channel, cl, sp,
					       class, sw_ctx, false, sw_ctx);
	if (IS_ERR(context->hwctx)) {
		host1x_channel_put(context->channel);
		return PTR_ERR(context->hwctx);
	}

	return 0;
}

static void gr2d_close_channel(struct tegra_drm_context *context)
{
	gr2d_release_context(context);
	host1x_context_put(context->hwctx);
	host1x_channel_put(context->channel);
}

static int gr2d_is_addr_reg(struct device *dev, u32 offset)
{
	struct gr2d *gr2d = dev_get_drvdata(dev);

	if (offset >= GR2D_NUM_REGS)
		return 0;

	if (test_bit(offset, gr2d->addr_regs))
		return 1;

	return 0;
}

static const struct tegra_drm_client_ops gr2d_ops = {
	.open_channel = gr2d_open_channel,
	.close_channel = gr2d_close_channel,
	.is_addr_reg = gr2d_is_addr_reg,
	.submit = tegra_drm_submit,
};

static const struct of_device_id gr2d_match[] = {
	{ .compatible = "nvidia,tegra30-gr2d" },
	{ .compatible = "nvidia,tegra20-gr2d" },
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

static int gr2d_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct host1x_syncpt **syncpts;
	struct gr2d *gr2d;
	unsigned int i;
	int err;

	gr2d = devm_kzalloc(dev, sizeof(*gr2d), GFP_KERNEL);
	if (!gr2d)
		return -ENOMEM;

	syncpts = devm_kzalloc(dev, sizeof(*syncpts), GFP_KERNEL);
	if (!syncpts)
		return -ENOMEM;

	gr2d->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(gr2d->clk)) {
		dev_err(dev, "cannot get clock\n");
		return PTR_ERR(gr2d->clk);
	}

	err = clk_prepare_enable(gr2d->clk);
	if (err) {
		dev_err(dev, "cannot turn on clock\n");
		return err;
	}

	gr2d->rst = devm_reset_control_get(dev, NULL);
	if (IS_ERR(gr2d->rst)) {
		dev_err(dev, "cannot get reset\n");
		return PTR_ERR(gr2d->rst);
	}

	INIT_LIST_HEAD(&gr2d->client.base.list);
	gr2d->client.base.ops = &gr2d_client_ops;
	gr2d->client.base.dev = dev;
	gr2d->client.base.class = HOST1X_CLASS_GR2D_G2_CTX2;
	gr2d->client.base.module = HOST1X_MODULE_GR2D;
	gr2d->client.base.syncpts = syncpts;
	gr2d->client.base.num_syncpts = 1;

	INIT_LIST_HEAD(&gr2d->client.list);
	gr2d->client.ops = &gr2d_ops;

	err = gr2d_reset(&gr2d->client.base);
	if (err)
		return err;

	err = host1x_client_register(&gr2d->client.base);
	if (err < 0) {
		dev_err(dev, "failed to register host1x client: %d\n", err);
		clk_disable_unprepare(gr2d->clk);
		return err;
	}

	/* initialize address register map */
	for (i = 0; i < ARRAY_SIZE(gr2d_addr_regs); i++)
		set_bit(gr2d_addr_regs[i], gr2d->addr_regs);

	platform_set_drvdata(pdev, gr2d);

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

	clk_disable_unprepare(gr2d->clk);

	return 0;
}

struct platform_driver tegra_gr2d_driver = {
	.driver = {
		.name = "tegra-gr2d",
		.of_match_table = gr2d_match,
	},
	.probe = gr2d_probe,
	.remove = gr2d_remove,
};
