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

#include "commands-pool.h"
#include "drm.h"
#include "gem.h"
#include "gr3d.h"

#define INVALID_ADDR	0x666DEAD0

#define OPCODE_SETCL(classid)				\
	((0x0 << 28) | (classid << 6))

#define OPCODE_INCR(offset, count)			\
	((0x1 << 28) | ((offset) << 16) | count)

#define OPCODE_NONINCR(offset, count)			\
	((0x2 << 28) | ((offset) << 16) | count)

#define OPCODE_MASK(offset, mask)			\
	((0x3 << 28) | ((offset) << 16) | mask)

#define OPCODE_IMM(offset, data)			\
	((0x4 << 28) | ((offset) << 16) | data)

struct gr3d {
	struct tegra_drm_client client;
	struct tegra_drm_commands_pool *commands_pool;
	struct clk *clk_secondary;
	struct clk *clk;
	struct reset_control *rst_secondary;
	struct reset_control *rst;
	struct host1x_syncpt *syncpt;

	DECLARE_BITMAP(addr_regs, GR3D_NUM_REGS);
};

enum gr3d_bind_type {
	DRM_TEGRA_3D_TYPE_IDX,
	DRM_TEGRA_3D_TYPE_ATTR,
	DRM_TEGRA_3D_TYPE_TEX,
	DRM_TEGRA_3D_TYPE_RT,
	DRM_TEGRA_3D_TYPE_SPILL,
};

struct gr3d_commands_public {
	u32 r_0x00c[1 + 10];
	u32 r_0x12x[1 +  5];
	u32 r_0x200[1 +  5];
	u32 r_0x209[1 +  9];
	u32 r_0x340[1 + 38];
	u32 r_0x400[1 + 18];
	u32 r_0x542[1 +  5];
	u32 r_0x500[1 +  4];
	u32 r_0x608[1 +  4];
	u32 r_0x740[1 +  2];
	u32 r_0x902[1 +  2];
	u32 r_0xa00[1 + 13];
	u32 r_0xe20[1 + 11];

	u32 r_0x205[1 +    1];	// vertex program instructions ID
	u32 r_0x206[1 + 1024];	// vertex program
	u32 r_0x207[1 +    1];	// vertex program constants ID
	u32 r_0x208[1 + 1024];	// vertex constants
	u32 r_0x300[1 +   64];	// linker program
	u32 r_0x520[1 +   32];	// fragment PSEQ eng. instructions
	u32 r_0x540[1 +    1];	// fragment PSEQ instructions ID
	u32 r_0x541[1 +   64];	// fragment PSEQ instructions
	u32 r_0x600[1 +    1];	// fragment MFU instructions schedule ID
	u32 r_0x601[1 +   64];	// fragment MFU instructions schedule
	u32 r_0x603[1 +    1];	// fragment MFU instructions ID
	u32 r_0x604[1 +  128];	// fragment MFU instructions
	u32 r_0x700[1 +    1];	// fragment TEX instructions ID
	u32 r_0x701[1 +   64];	// fragment TEX instructions
	u32 r_0x800[1 +    1];	// fragment ALU instructions schedule ID
	u32 r_0x801[1 +   64];	// fragment ALU instructions schedule
	u32 r_0x803[1 +    1];	// fragment ALU instructions ID
	u32 r_0x804[1 +  512];	// fragment ALU instructions
	u32 r_0x805[1 +    1];	// fragment ALU instructions complement ID
	u32 r_0x806[1 +   64];	// fragment ALU instructions complement
	u32 r_0x820[1 +   32];	// fragment constants
	u32 r_0x900[1 +    1];	// fragment DW instructions ID
	u32 r_0x901[1 +   64];	// fragment DW instructions
};

struct gr3d_commands_private {
	u32 r_0x100[1 + 32];	// vertex attributes descriptors
	u32 r_0x121[1 +  1];	// vertex indices pointer
	u32 r_0x710[1 + 48];	// fragment textures descriptors
	u32 r_0xe00[1 + 32];	// fragment render target descriptors
	u32 r_0xe2a[1 +  1];	// spilling buffer pointer
};

struct gr3d_commands {
	/* setup insecure-generic registers state */
	struct gr3d_commands_public public;

	/* setup secure-validated registers state */
	struct gr3d_commands_private private;

	/* trigger drawing */
	u32 draw_primitives[2];

	/* increment syncpoint on draw completion */
	u32 syncpt_incr;
};

struct gr3d_context_binding {
	union {
		struct {
			struct tegra_bo *spill;
			struct tegra_bo *indices;
			struct tegra_bo *rt[16];
			struct tegra_bo *tex[16];
			struct tegra_bo *attrs[16];
		};

		struct tegra_bo *bos[56];
	};
};

struct gr3d_context {
	struct gr3d_context_binding binding;
};

struct gr3d_callback_data {
	struct tegra_drm_context *context;
	struct tegra_drm_commands_bo *commands_bo;
	struct gr3d_context ctx3d;
};

static inline struct gr3d *to_gr3d(struct tegra_drm_client *client)
{
	return container_of(client, struct gr3d, client);
}

static struct gr3d_context *gr3d_init_context(
					struct gr3d_context *ctx3d,
					struct gr3d_commands_private *commands)
{
	unsigned int i;

	/* initialize vertex attributes pointers */
	commands->r_0x100[0] = OPCODE_INCR(0x100, 32);
	for (i = 1; i <= 32; i += 2)
		commands->r_0x100[i] = INVALID_ADDR;

	/* initialize vertex indices pointer */
	commands->r_0x121[0] = OPCODE_NONINCR(0x121, 1);
	commands->r_0x121[1] = INVALID_ADDR;

	/* initialize fragment textures pointers */
	commands->r_0x710[0] = OPCODE_INCR(0x710, 48);
	for (i = 1; i <= 16; i++)
		commands->r_0x710[i] = INVALID_ADDR;

	/* initialize fragment render target pointers */
	commands->r_0xe00[0] = OPCODE_INCR(0xe00, 32);
	for (i = 1; i <= 16; i++)
		commands->r_0xe00[i] = INVALID_ADDR;

	/* initialize spilling buffer pointer */
	commands->r_0xe2a[0] = OPCODE_NONINCR(0xe2a, 1);
	commands->r_0xe2a[1] = INVALID_ADDR;

	return ctx3d;
}

static int gr3d_bind_bo(struct gr3d_context *ctx3d,
			struct drm_file *file,
			struct gr3d_commands_private *commands,
			unsigned int index,
			u32 handle, u32 offset, u32 desc1, u32 desc2,
			enum gr3d_bind_type type)
{
	struct drm_gem_object *gem;
	struct tegra_bo *bo;
	dma_addr_t dma_addr;

	gem = drm_gem_object_lookup(file, handle);
	if (!gem)
		return -ENOENT;

	bo = to_tegra_bo(gem);

	/* TODO validate offset/size based on format descriptor */
	if (WARN_ON(offset >= gem->size)) {
		drm_gem_object_unreference_unlocked(gem);
		return -EINVAL;
	}

	/* TODO check overall BO's size and block on ENOMEM */
	dma_addr = tegra_bo_pin(&bo->base, NULL) + offset;
	if (!dma_addr) {
		drm_gem_object_unreference_unlocked(gem);
		return -ENOMEM;
	}

	switch (type) {
	case DRM_TEGRA_3D_TYPE_IDX:
		ctx3d->binding.indices = bo;
		commands->r_0x121[1] = dma_addr;

		break;

	case DRM_TEGRA_3D_TYPE_ATTR:
		ctx3d->binding.attrs[index] = bo;
		commands->r_0x100[1 + index * 2] = dma_addr;
		commands->r_0x100[2 + index * 2] = desc1;

		break;

	case DRM_TEGRA_3D_TYPE_TEX:
		ctx3d->binding.tex[index] = bo;
		commands->r_0x710[ 1 + index] = dma_addr;
		commands->r_0x710[17 + index * 2 + 0] = desc1;
		commands->r_0x710[17 + index * 2 + 1] = desc2;

		break;

	case DRM_TEGRA_3D_TYPE_RT:
		ctx3d->binding.rt[index] = bo;
		commands->r_0xe00[ 1 + index] = dma_addr;
		commands->r_0xe00[17 + index] = desc1;

		break;

	case DRM_TEGRA_3D_TYPE_SPILL:
		/* TODO */

		return -EINVAL;

	default:
		return -EINVAL;
	}

	return 0;
}

static void gr3d_unbind_context(struct gr3d_context *ctx3d)
{
	struct gr3d_context_binding *binding = &ctx3d->binding;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(binding->bos); i++) {
		struct tegra_bo *bo = binding->bos[i];

		if (!bo)
			continue;

		tegra_bo_unpin(&bo->base, NULL);
		drm_gem_object_unreference_unlocked(&bo->gem);
	}
}

static int gr3d_bind_context(void *ptr, u32 **end,
			     struct gr3d_context *ctx3d,
			     struct drm_tegra_3d_submit *args,
			     struct drm_file *file)
{
	struct gr3d_commands_private *commands = ptr;
	unsigned int i;
	int err;

	gr3d_init_context(ctx3d, commands);

	if (args->spill_surf.enabled) {
		err = gr3d_bind_bo(ctx3d, file, commands, 0,
				   args->spill_surf.handle,
				   args->spill_surf.offset,
				   0x00000000,
				   0x00000000,
				   DRM_TEGRA_3D_TYPE_SPILL);
		if (err)
			goto err_unbind;
	}

	if (args->indices.enabled) {
		err = gr3d_bind_bo(ctx3d, file, commands, 0,
				   args->indices.handle,
				   args->indices.offset,
				   0x00000000,
				   0x00000000,
				   DRM_TEGRA_3D_TYPE_IDX);
		if (err)
			goto err_unbind;
	}

	for (i = 0; i < ARRAY_SIZE(args->attributes); i++) {
		if (!args->attributes[i].enabled)
			continue;

		err = gr3d_bind_bo(ctx3d, file, commands, i,
				   args->attributes[i].handle,
				   args->attributes[i].offset,
				   args->attributes[i].desc,
				   0x00000000,
				   DRM_TEGRA_3D_TYPE_ATTR);
		if (err)
			goto err_unbind;
	}

	for (i = 0; i < ARRAY_SIZE(args->textures); i++) {
		if (!args->textures[i].enabled)
			continue;

		err = gr3d_bind_bo(ctx3d, file, commands, i,
				   args->textures[i].handle,
				   args->textures[i].offset,
				   args->textures[i].desc1,
				   args->textures[i].desc2,
				   DRM_TEGRA_3D_TYPE_TEX);
		if (err)
			goto err_unbind;
	}

	for (i = 0; i < ARRAY_SIZE(args->render_targets); i++) {
		if (!args->render_targets[i].enabled)
			continue;

		err = gr3d_bind_bo(ctx3d, file, commands, i,
				   args->render_targets[i].handle,
				   args->render_targets[i].offset,
				   args->render_targets[i].desc,
				   0x00000000,
				   DRM_TEGRA_3D_TYPE_RT);
		if (err)
			goto err_unbind;
	}

	*end = ptr + sizeof(*commands);

	return 0;

err_unbind:
	gr3d_unbind_context(ctx3d);

	return err;
}

static void gr3d_submit_done(struct host1x_job *job)
{
	struct gr3d_callback_data *cb = job->callback_data;

	gr3d_unbind_context(&cb->ctx3d);

	tegra_drm_commands_pool_free(cb->commands_bo);
	tegra_drm_context_put_channel(cb->context);

	kfree(cb);
}

static int gr3d_copy_userspace(void *start, u32 **end,
			       struct drm_tegra_3d_submit *args,
			       struct drm_file *file)
{
	struct drm_tegra_3d_vertex_consts __user *user_vertex_consts;
	struct drm_tegra_3d_vertex_program __user *user_vertex_prog;
	struct drm_tegra_3d_linker_program __user *user_linker_prog;
	struct drm_tegra_3d_fragment_consts __user *user_fragment_consts;
	struct drm_tegra_3d_fragment_program __user *user_fragment_prog;
	struct drm_tegra_3d_regs __user *user_regs;
	struct drm_tegra_3d_regs regs;
	u32 *commands = start;

	user_regs = u64_to_user_ptr(args->regs_pointer);
	user_vertex_prog = u64_to_user_ptr(args->vertex_prog_pointer);
	user_linker_prog = u64_to_user_ptr(args->linker_prog_pointer);
	user_fragment_prog = u64_to_user_ptr(args->fragment_prog_pointer);
	user_vertex_consts = u64_to_user_ptr(args->vertex_consts_pointer);
	user_fragment_consts = u64_to_user_ptr(args->fragment_consts_pointer);

#define GR3D_COPY_REGS1(offt)						\
	*commands++ = OPCODE_INCR(offt, ARRAY_SIZE(regs.r_##offt));	\
	memcpy(commands, regs.r_##offt,					\
			ARRAY_SIZE(regs.r_##offt) * sizeof(u32));	\
	commands += ARRAY_SIZE(regs.r_##offt)

#define GR3D_COPY_REGS2(offt, ptr, num)					\
	*commands++ = OPCODE_INCR(offt, num);				\
	if (copy_from_user(commands, &user_##ptr->r_##offt,		\
			   num * sizeof(u32))) {			\
		return -EFAULT;						\
	}								\
	commands += num

#define GR3D_COPY_REGS3(offt, ptr, num)					\
	*commands++ = OPCODE_IMM(offt - 1, 0);				\
	*commands++ = OPCODE_NONINCR(offt, num);			\
	if (copy_from_user(commands, &user_##ptr->r_##offt,		\
			   num * sizeof(u32))) {			\
		return -EFAULT;						\
	}								\
	commands += num

	/* copy public-generic registers */
	if (copy_from_user(&regs, user_regs, sizeof(regs)))
		return -EFAULT;

	GR3D_COPY_REGS1(0x00c);

	*commands++ = OPCODE_MASK(0x120, 0x75);
	*commands++ = regs.r_0x120[0];
	*commands++ = regs.r_0x122[0];
	*commands++ = regs.r_0x124[0];
	*commands++ = regs.r_0x124[1];
	*commands++ = regs.r_0x124[2];

	GR3D_COPY_REGS1(0x200);
	GR3D_COPY_REGS1(0x209);
	GR3D_COPY_REGS1(0x340);
	GR3D_COPY_REGS1(0x400);
	GR3D_COPY_REGS1(0x542);
	GR3D_COPY_REGS1(0x500);
	GR3D_COPY_REGS1(0x608);
	GR3D_COPY_REGS1(0x740);
	GR3D_COPY_REGS1(0x902);
	GR3D_COPY_REGS1(0xa00);
	GR3D_COPY_REGS1(0xe20);

	GR3D_COPY_REGS3(0x206, vertex_prog, args->vp_instructions_num);
	GR3D_COPY_REGS3(0x208, vertex_consts, args->vp_consts_num);
	GR3D_COPY_REGS2(0x300, linker_prog, args->lp_instructions_num);
	GR3D_COPY_REGS2(0x520, fragment_prog, args->fp_pseq_eng_num);
	GR3D_COPY_REGS3(0x541, fragment_prog, args->fp_pseq_num);
	GR3D_COPY_REGS3(0x601, fragment_prog, args->fp_mfu_sched_num);
	GR3D_COPY_REGS3(0x604, fragment_prog, args->fp_mfu_num);
	GR3D_COPY_REGS3(0x701, fragment_prog, args->fp_tex_num);
	GR3D_COPY_REGS3(0x801, fragment_prog, args->fp_alu_sched_num);
	GR3D_COPY_REGS3(0x804, fragment_prog, args->fp_alu_num);
	GR3D_COPY_REGS3(0x806, fragment_prog, args->fp_alu_comp_num);
	GR3D_COPY_REGS2(0x820, fragment_consts, args->fp_consts_num);
	GR3D_COPY_REGS3(0x901, fragment_prog, args->fp_dw_num);

	*end = commands;

	return 0;
}

static int gr3d_finalize_context(struct gr3d_commands *commands,
				 struct drm_tegra_3d_submit *args,
				 u32 *ptr, u32 syncpt_id)
{
	/* trigger drawing */
	*ptr++ = OPCODE_NONINCR(0x123, 1);
	*ptr++ = args->draw_primitives;

	/* increment syncpoint on draw completion */
	*ptr++ = OPCODE_IMM(0x000, 1 << 8 | syncpt_id);

	return ptr - (u32*)commands;
}

static int gr3d_submit(struct tegra_drm_context *context,
		       struct drm_tegra_3d_submit *args,
		       struct drm_file *file)
{
	struct gr3d *gr3d = to_gr3d(context->client);
	struct gr3d_callback_data *cb;
	struct gr3d_commands *commands;
	struct tegra_drm_commands_bo *commands_bo;
	struct host1x_job *job;
	unsigned int words;
	u32 syncpt_id;
	u32 *ptr;
	int err;

	if (WARN_ON(args->vp_instructions_num > 1024 ||
	    args->lp_instructions_num > 64 ||
	    args->fp_consts_num > 1024 ||
	    args->fp_mfu_sched_num > 64 ||
	    args->fp_alu_sched_num > 64 ||
	    args->fp_alu_comp_num > 64 ||
	    args->fp_pseq_eng_num > 32 ||
	    args->fp_consts_num > 32 ||
	    args->fp_pseq_num > 64 ||
	    args->fp_mfu_num > 128 ||
	    args->fp_alu_num > 512 ||
	    args->fp_tex_num > 64 ||
	    args->fp_dw_num > 64))
		return -EINVAL;

	commands_bo = tegra_drm_commands_pool_alloc(gr3d->commands_pool);
	if (IS_ERR(commands_bo))
		return PTR_ERR(commands_bo);

	err = tegra_drm_context_get_channel(context);
	if (err < 0)
		goto err_free_cmds;

	job = host1x_job_alloc(context->channel, 1, 0, 0);
	if (!job) {
		err = -ENOMEM;
		goto err_put_ch;
	}

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb) {
		err = -ENOMEM;
		goto err_put_job;
	}

	cb->context = context;
	cb->commands_bo = commands_bo;

	commands = commands_bo->vaddr;
	syncpt_id = host1x_syncpt_id(context->client->base.syncpts[0]);

	/* setup public state */
	err = gr3d_copy_userspace(&commands->public, &ptr, args, file);
	if (err)
		goto err_free_cb;

	/* setup private-secure state */
	err = gr3d_bind_context(ptr, &ptr, &cb->ctx3d, args, file);
	if (err)
		goto err_free_cb;

	words = gr3d_finalize_context(commands, args, ptr, syncpt_id);

	/* TODO validate render targets */

	host1x_job_add_gather2(job, &commands_bo->base, words, 0,
			       commands_bo->dma);

	job->class		= HOST1X_CLASS_GR3D;
	job->syncpt_incrs	= 1;
	job->syncpt_id		= syncpt_id;
	job->timeout		= 500;
	job->client		= &context->client->base;
	job->serialize		= true;
	job->done		= gr3d_submit_done;
	job->callback_data	= cb;

	err = host1x_job_submit(job);
	if (err)
		goto err_unbind;

	args->fence = job->syncpt_end;

	return 0;

err_unbind:
	gr3d_unbind_context(&cb->ctx3d);

err_free_cb:
	kfree(cb);

err_put_job:
	host1x_job_put(job);

err_free_cmds:
	tegra_drm_commands_pool_free(commands_bo);

err_put_ch:
	tegra_drm_context_put_channel(context);

	return err;
}

static int gr3d_init(struct host1x_client *client)
{
	struct tegra_drm_client *drm = host1x_to_drm_client(client);
	struct drm_device *dev = dev_get_drvdata(client->parent);
	unsigned long flags = HOST1X_SYNCPT_HAS_BASE;
	struct gr3d *gr3d = to_gr3d(drm);
	size_t block_size;
	int err;

	client->syncpts[0] = host1x_syncpt_request(client->dev, flags);
	if (!client->syncpts[0])
		return -ENOMEM;

	block_size = sizeof(struct gr3d_commands);
	gr3d->commands_pool = tegra_drm_commands_pool_create(dev, block_size,
							     4, 3);
	if (!gr3d->commands_pool) {
		err = -ENOMEM;
		goto err_free_syncpt;
	}

	err = tegra_drm_register_client(dev->dev_private, drm);
	if (err)
		goto err_free_pool;

	return 0;

err_free_syncpt:
	host1x_syncpt_free(client->syncpts[0]);

err_free_pool:
	tegra_drm_commands_pool_destroy(gr3d->commands_pool);

	return err;
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

	tegra_drm_commands_pool_destroy(gr3d->commands_pool);
	host1x_syncpt_free(client->syncpts[0]);

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

static int gr3d_open_channel(struct tegra_drm_client *client,
			     struct tegra_drm_context *context)
{
	context->syncpt = client->base.syncpts[0];

	return 0;
}

static void gr3d_close_channel(struct tegra_drm_context *context)
{
}

static int gr3d_is_addr_reg(struct device *dev, u32 class, u32 offset)
{
	struct gr3d *gr3d = dev_get_drvdata(dev);

	switch (class) {
	case HOST1X_CLASS_HOST1X:
		if (offset == 0x2b)
			return 1;

		break;

	case HOST1X_CLASS_GR3D:
		if (offset >= GR3D_NUM_REGS)
			break;

		if (test_bit(offset, gr3d->addr_regs))
			return 1;

		break;
	}

	return 0;
}

static const struct tegra_drm_client_ops gr3d_ops = {
	.open_channel = gr3d_open_channel,
	.close_channel = gr3d_close_channel,
	.is_addr_reg = gr3d_is_addr_reg,
	.submit = tegra_drm_submit,
	.submit_3d = gr3d_submit,
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
