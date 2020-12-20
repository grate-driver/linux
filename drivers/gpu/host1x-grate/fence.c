/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/host1x-grate.h>

DEFINE_SPINLOCK(host1x_syncpts_lock);
EXPORT_SYMBOL(host1x_syncpts_lock);

static struct kmem_cache *host1x_fence_slab;
static DEFINE_MUTEX(host1x_slab_lock);

static inline int host1x_init_fence_slab(void)
{
	int ret = 0;

	if (host1x_fence_slab)
		return 0;

	mutex_lock(&host1x_slab_lock);

	if (!host1x_fence_slab) {
		host1x_fence_slab = KMEM_CACHE(host1x_fence, SLAB_HWCACHE_ALIGN);
		if (!host1x_fence_slab)
			ret = -ENOMEM;
	}

	mutex_unlock(&host1x_slab_lock);

	return ret;
}

static const char *host1x_fence_get_driver_name(struct dma_fence *f)
{
	return "host1x";
}

static const char *host1x_fence_get_timeline_name(struct dma_fence *f)
{
	return "sync point";
}

static void host1x_fence_free(struct rcu_head *rcu)
{
	struct dma_fence *f = container_of(rcu, struct dma_fence, rcu);
	struct host1x_fence *fence = container_of(f, struct host1x_fence, base);

	kmem_cache_free(host1x_fence_slab, fence);
}

static void host1x_fence_release(struct dma_fence *f)
{
	call_rcu(&f->rcu, host1x_fence_free);
}

const struct dma_fence_ops host1x_fence_ops = {
	.get_driver_name = host1x_fence_get_driver_name,
	.get_timeline_name = host1x_fence_get_timeline_name,
	.release = host1x_fence_release,
};
EXPORT_SYMBOL(host1x_fence_ops);

struct dma_fence *host1x_fence_create(struct host1x_channel *chan,
				      struct host1x_syncpt *syncpt,
				      u32 threshold, u64 context)
{
	struct host1x *host = syncpt->host;
	struct host1x_fence *fence;
	unsigned long flags;
	int err;

	err = host1x_init_fence_slab();
	if (err)
		return NULL;

	fence = kmem_cache_alloc(host1x_fence_slab, GFP_KERNEL);
	if (!fence)
		return NULL;

	fence->syncpt_thresh = threshold;
	fence->channel = chan;

	/*
	 * Note that we expect here that fences are created in chronological
	 * order, i.e. threshold value of previous fence is lower than the
	 * value of this fence. Otherwise fence's timeline order won't be
	 * correct.
	 */
	dma_fence_init(&fence->base, &host1x_fence_ops, &host1x_syncpts_lock,
		       context, atomic_inc_return(&host->fence_seq));

	/* fence won't be released until sync point permits that */
	dma_fence_get(&fence->base);

	spin_lock_irqsave(&host1x_syncpts_lock, flags);

	/* attach fence to the sync point */
	list_add_tail(&fence->list, &syncpt->fences);
	/* mark sync point as active */
	set_bit(syncpt->id, host->active_syncpts);

	spin_unlock_irqrestore(&host1x_syncpts_lock, flags);

	return &fence->base;
}
EXPORT_SYMBOL(host1x_fence_create);
