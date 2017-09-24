/*
 * Copyright 2017 Dmitry Osipenko <digetx@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/iova.h>
#include <linux/slab.h>

#include "context.h"
#include "dev.h"

/*
 * Bump reference count of the recently scheduled context.
 */
void host1x_context_get_recent(struct host1x_channel *ch)
{
	spin_lock(&ch->context_lock);

	host1x_context_get(ch->recent_ctx);

	spin_unlock(&ch->context_lock);
}

/*
 * Update channels recently scheduled context entry.
 */
void host1x_context_update_recent(struct host1x_channel *ch,
				  struct host1x_context *ctx,
				  bool release)
{
	spin_lock(&ch->context_lock);

	if (release && ch->recent_ctx == ctx)
		ch->recent_ctx = NULL;

	if (!release)
		ch->recent_ctx = ctx;

	spin_unlock(&ch->context_lock);
}

/*
 * Perform HW context data store by reading channels output FIFO
 * and writing the read data to contexts BO.
 */
void host1x_context_store(struct host1x_context *ctx)
{
	struct host1x_channel *ch = ctx->channel;
	struct host1x *host = dev_get_drvdata(ch->dev->parent);

	dev_dbg(ch->dev, "%s: CTX %p class 0x%X\n",
		__func__, ctx, ctx->class);

	host1x_hw_channel_read_inddata(host, ch,
				       ctx->bo_vaddr + ctx->bo_offset,
				       ctx->words_num);

	if (ctx->ops->debug)
		ctx->ops->debug(ctx->client, ctx->bo_vaddr);

	host1x_syncpt_incr(ctx->sp);
}

/*
 * Releases context once all jobs associated with this context are
 * completed and channel is closed.
*/
static void host1x_release_context(struct kref *kref)
{
	struct host1x_context *ctx =
		container_of(kref, struct host1x_context, ref);
	struct host1x_channel *ch = ctx->channel;
	struct host1x_context *recent_ctx = ch->recent_ctx;
	struct host1x *host = dev_get_drvdata(ch->dev->parent);
	size_t aligned_size;

	dev_dbg(ch->dev, "%s: CTX %p class 0x%X, channels recent CTX %p "
			 "class 0x%X\n",
		__func__, ctx, ctx->class, recent_ctx,
		recent_ctx ? recent_ctx->class : 0);

	/*
	 * Check whether it's the latest channel context owner being
	 * released now, reset channels context entry if it is so.
	 */
	host1x_context_update_recent(ch, ctx, true);

	if (!ctx->bo)
		goto free_ctx;

	if (host->domain) {
		aligned_size = host1x_bo_size(ctx->bo);
		aligned_size = iova_align(&host->iova, aligned_size);

		iommu_unmap(host->domain, ctx->bo_dma, aligned_size);
		free_iova(&host->iova,
			  iova_pfn(&host->iova, ctx->bo_dma));
	}

	host1x_bo_munmap(ctx->bo, ctx->bo_vaddr);
	host1x_bo_unpin(ctx->bo, ctx->sgt);
	host1x_bo_put(ctx->bo);

free_ctx:
	kfree(ctx->restore_data);
	kfree(ctx->store_data);
	kfree(ctx);
}

struct host1x_context *host1x_context_get(struct host1x_context *ctx)
{
	if (ctx) {
		dev_dbg(ctx->channel->dev, "%s: CTX %p class 0x%X\n",
			__func__, ctx, ctx->class);

		kref_get(&ctx->ref);
	}

	return ctx;
}
EXPORT_SYMBOL(host1x_context_get);

void host1x_context_put(struct host1x_context *ctx)
{
	if (ctx) {
		dev_dbg(ctx->channel->dev, "%s: CTX %p class 0x%X\n",
			__func__, ctx, ctx->class);

		kref_put(&ctx->ref, host1x_release_context);
	}
}
EXPORT_SYMBOL(host1x_context_put);

/*
 * Returns true if context restore is needed, otherwise returns false.
 */
bool host1x_context_restore_required(struct host1x_context *ctx)
{
	struct host1x_context *recent_ctx = ctx->channel->recent_ctx;
	bool hw_restore = !!ctx->restore_pushes;
	bool restore = (hw_restore && (!ctx->inited || recent_ctx != ctx));

	dev_dbg(ctx->channel->dev, "%s: CTX %p class 0x%X, recent CTX %p "
				   "class 0x%X (%s)\n",
		__func__, ctx, ctx->class, recent_ctx,
		recent_ctx ? recent_ctx->class : 0,
		restore ? "true" : "false");

	/* assume that context would be initialized shortly */
	ctx->inited = true;

	return restore;
}

/*
 * Returns true if context differs from the current channels one and
 * context store is required for context switching, otherwise returns false.
 */
bool host1x_context_store_required(struct host1x_context *ctx)
{
	struct host1x_context *recent_ctx = ctx->channel->recent_ctx;
	bool ctx_xchg = (recent_ctx && recent_ctx != ctx);
	bool hw_store = (ctx_xchg && ctx->hw_store);
	bool sw_store = (ctx_xchg && ctx->sw_store);
	bool store = (hw_store || sw_store);

	dev_dbg(ctx->channel->dev, "%s: CTX %p class 0x%X, recent CTX %p "
				   "class 0x%X (%s%s)\n",
		__func__, ctx, ctx->class, recent_ctx,
		recent_ctx ? recent_ctx->class : 0,
		store ? "true" : "false",
		hw_store ? " HW" : (sw_store ? " SW" : ""));

	return store;
}

/*
 * Returns Host1x class ID associated with this context or client's base
 * class ID.
 */
u32 host1x_context_class(struct host1x_client *client,
			 struct host1x_context *ctx)
{
	return ctx ? ctx->class : client->class;
}
EXPORT_SYMBOL(host1x_context_class);

static int initialize_ctx(struct host1x_context *ctx)
{
	struct host1x *host = dev_get_drvdata(ctx->channel->dev->parent);
	struct iommu_domain *domain = host->domain;
	struct iova *alloc = NULL;
	size_t aligned_size;
	unsigned long shift;
	int err;

	ctx->bo_phys = host1x_bo_pin(ctx->bo, &ctx->sgt);
	if (!ctx->bo_phys)
		return -EINVAL;

	if (domain) {
		aligned_size = host1x_bo_size(ctx->bo);
		aligned_size = iova_align(&host->iova, aligned_size);

		shift = iova_shift(&host->iova);
		alloc = alloc_iova(&host->iova, aligned_size >> shift,
				   host->iova_end >> shift, true);
		if (!alloc) {
			err = -ENOMEM;
			goto err_unpin;
		}

		if (iommu_map_sg(domain,
				 iova_dma_addr(&host->iova, alloc),
				 ctx->sgt->sgl, ctx->sgt->nents,
				 IOMMU_READ) < aligned_size) {
			err = -EINVAL;
			goto err_free_iova;
		}

		ctx->bo_dma = iova_dma_addr(&host->iova, alloc);
	} else {
		ctx->bo_dma = ctx->bo_phys;
	}

	ctx->bo_vaddr = host1x_bo_mmap(ctx->bo);
	if (!ctx->bo_vaddr) {
		err = -ENOMEM;
		goto err_iommu_unmap;
	}

	/*
	 * Initialize commands BO with a registers state that would be
	 * written to HW on the first submission of a job that uses this
	 * context.
	 */
	err = ctx->ops->initialize(ctx->client,
				   ctx->class,
				   ctx->bo_vaddr,
				   ctx->bo_dma,
				   &ctx->bo_offset,
				   &ctx->words_num,
				   &ctx->restore_data,
				   &ctx->store_data,
				   &ctx->restore_pushes,
				   &ctx->store_pushes);
	if (err)
		goto err_unmap;

	return 0;

err_unmap:
	host1x_bo_munmap(ctx->bo, ctx->bo_vaddr);

err_iommu_unmap:
	if (alloc)
		iommu_unmap(domain, ctx->bo_dma, aligned_size);

err_free_iova:
	if (alloc)
		__free_iova(&host->iova, alloc);

err_unpin:
	host1x_bo_unpin(ctx->bo, ctx->sgt);

	return err;
}

struct host1x_context *host1x_create_context(
			const struct host1x_context_ops *ops,
			struct host1x_channel *channel,
			struct host1x_client *client,
			struct host1x_syncpt *sp,
			enum host1x_class class,
			bool hw_restore,
			bool hw_store,
			bool sw_store)
{
	struct host1x_context *ctx;
	int err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	dev_dbg(client->dev, "%s: CTX %p class 0x%X\n",
		__func__, ctx, class);

	if (hw_store && sw_store) {
		err = -EINVAL;
		goto err_free;
	}

	kref_init(&ctx->ref);
	ctx->hw_store = hw_store;
	ctx->sw_store = sw_store;
	ctx->channel = channel;
	ctx->client = client;
	ctx->class = class;
	ctx->ops = ops;
	ctx->sp = sp;

	/*
	 * Bail out if context doesn't require any kind of store/restore,
	 * this is relevant to HW that can manage context switching by itself
	 * or a dedicated context register banks being used for this context.
	 */
	if (!hw_restore && !hw_store && !sw_store)
		return ctx;

	/* ask client to allocate context switching commands BO */
	err = ops->allocate(client, &ctx->bo);
	if (err)
		goto err_free;

	/* pin, map and setup "HW context switch" commands */
	err = initialize_ctx(ctx);
	if (err)
		goto err_put_bo;

	return ctx;

err_put_bo:
	host1x_bo_put(ctx->bo);

err_free:
	kfree(ctx);

	dev_err(client->dev, "Failed to create context %d\n", err);

	return ERR_PTR(err);
}
EXPORT_SYMBOL(host1x_create_context);
