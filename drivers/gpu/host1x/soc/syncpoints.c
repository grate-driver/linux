/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/bitmap.h>
#include <linux/interrupt.h>

static int host1x_soc_init_syncpts(struct host1x *host)
{
	int err;

	idr_init(&host->syncpts);
	init_completion(&host->syncpt_release_complete);

	/*
	 * Allocate active_syncpts, each bit represents one active
	 * sync point.
	 */
	host->active_syncpts = bitmap_zalloc(HOST1X_SYNCPTS_NUM, GFP_KERNEL);
	if (!host->active_syncpts) {
		err = -ENOMEM;
		goto err_destroy_idr;
	}

	/*
	 * Create cache for sync points to avoid stalls during allocations
	 * and to increase locality of the data.
	 */
	host->syncpts_slab = KMEM_CACHE(host1x_syncpt, 0);
	if (!host->syncpts_slab) {
		err = -ENOMEM;
		goto err_free_bitmap;
	}

	host1x_hw_init_syncpts(host);

	err = devm_request_irq(host->dev, host->syncpt_irq,
			       host1x_hw_syncpt_isr, 0,
			      "host1x_syncpt", host);
	if (err)
		goto err_destroy_slab;

	return 0;

err_destroy_slab:
	kmem_cache_destroy(host->syncpts_slab);
err_free_bitmap:
	bitmap_free(host->active_syncpts);
err_destroy_idr:
	idr_destroy(&host->syncpts);

	return err;
}

static void host1x_soc_deinit_syncpts(struct host1x *host)
{
	unsigned int i;

	/* shouldn't happen, all sync points must be released at this point */
	WARN_ON(!idr_is_empty(&host->syncpts));

	/* all sync points must be disabled now, but let's be extra paranoid */
	for (i = 0; i < HOST1X_SYNCPTS_NUM / 32; i++)
		host1x_hw_syncpt_set_interrupt(host, i, false);

	kmem_cache_destroy(host->syncpts_slab);
	bitmap_free(host->active_syncpts);
	idr_destroy(&host->syncpts);
}

static struct host1x_syncpt *
host1x_soc_syncpt_request(struct host1x *host)
{
	struct host1x_syncpt *syncpt;
	unsigned long flags;
	int ret;

	syncpt = kmem_cache_alloc(host->syncpts_slab, GFP_KERNEL);
	if (!syncpt)
		return ERR_PTR(-ENOMEM);
retry:
	idr_preload(GFP_KERNEL);
	spin_lock_irqsave(&host1x_syncpts_lock, flags);

	ret = idr_alloc(&host->syncpts, syncpt, 0, HOST1X_SYNCPTS_NUM,
			GFP_NOWAIT);
	if (ret == -ENOSPC)
		reinit_completion(&host->syncpt_release_complete);

	spin_unlock_irqrestore(&host1x_syncpts_lock, flags);
	idr_preload_end();

	if (ret == -ENOSPC) {
		ret = wait_for_completion_interruptible(
						&host->syncpt_release_complete);
		if (ret == 0)
			goto retry;
	}

	if (ret < 0) {
		kmem_cache_free(host->syncpts_slab, syncpt);
		return ERR_PTR(ret);
	}

	INIT_LIST_HEAD(&syncpt->fences);
	kref_init(&syncpt->refcount);
	syncpt->host = host;
	syncpt->id = ret;

	return syncpt;
}

static inline void
host1x_soc_syncpt_detach_fences_locked(struct host1x_syncpt *syncpt)
{
	struct host1x_fence *fence, *tmp;

	list_for_each_entry_safe(fence, tmp, &syncpt->fences, list) {
		list_del(&fence->list);
		dma_fence_put(&fence->base);
	}
}

static void host1x_soc_syncpt_detach_fences(struct host1x_syncpt *syncpt)
{
	unsigned long flags;

	spin_lock_irqsave(&host1x_syncpts_lock, flags);
	host1x_soc_syncpt_detach_fences_locked(syncpt);
	spin_unlock_irqrestore(&host1x_syncpts_lock, flags);
}

static inline void
host1x_soc_syncpt_reset_locked(struct host1x_syncpt *syncpt, int error)
{
	struct host1x *host = syncpt->host;
	struct host1x_fence *fence, *tmp;

	host1x_hw_syncpt_set_interrupt(host, syncpt->id, false);
	host1x_hw_syncpt_set_value(host, syncpt->id, 0);
	host1x_hw_syncpt_set_threshold(host, syncpt->id, 1);
	host1x_hw_syncpt_clr_intr_sts(host, syncpt->id);

	/* walk up pending fences and error out them */
	list_for_each_entry_safe(fence, tmp, &syncpt->fences, list) {

		dma_fence_set_error(&fence->base, error);
		dma_fence_signal_locked(&fence->base);
		list_del(&fence->list);
		dma_fence_put(&fence->base);
	}

	clear_bit(syncpt->id, host->active_syncpts);
}

static void host1x_soc_syncpt_reset(struct host1x_syncpt *syncpt, int error)
{
	unsigned long flags;

	spin_lock_irqsave(&host1x_syncpts_lock, flags);
	host1x_soc_syncpt_reset_locked(syncpt, error);
	spin_unlock_irqrestore(&host1x_syncpts_lock, flags);
}

static void host1x_soc_syncpt_release(struct kref *kref)
{
	struct host1x_syncpt *syncpt = container_of(kref, struct host1x_syncpt,
						    refcount);
	struct host1x *host = syncpt->host;
	unsigned long flags;

	spin_lock_irqsave(&host1x_syncpts_lock, flags);

	/* shouldn't happen, sync point must be idling at this point */
	if (WARN_ON_ONCE(!list_empty(&syncpt->fences)))
		host1x_soc_syncpt_reset_locked(syncpt, -ECANCELED);

	/* recycle sync point */
	idr_remove(&host->syncpts, syncpt->id);

	spin_unlock_irqrestore(&host1x_syncpts_lock, flags);

	complete(&host->syncpt_release_complete);
	kmem_cache_free(host->syncpts_slab, syncpt);
}

static void
host1x_soc_syncpt_set_interrupt(struct host1x_syncpt *syncpt, bool enabled)
{
	struct host1x *host = syncpt->host;
	unsigned long flags;

	spin_lock_irqsave(&host1x_syncpts_lock, flags);
	host1x_hw_syncpt_set_interrupt(host, syncpt->id, enabled);
	spin_unlock_irqrestore(&host1x_syncpts_lock, flags);
}

static u32 host1x_soc_syncpt_read(struct host1x_syncpt *syncpt)
{
	return host1x_hw_syncpt_value(syncpt->host, syncpt->id);
}

static void
host1x_soc_dump_syncpt_by_id(struct host1x_dbg_output *o,
			     struct host1x *host,
			     unsigned int id)
{
	struct host1x_syncpt *syncpt;
	u32 value = host1x_hw_syncpt_value(host, id);
	u32 thresh = host1x_hw_syncpt_thresh(host, id);
	bool status = host1x_hw_syncpt_intr_status(host, id);
	unsigned long flags;
	char user_name[256];

	spin_lock_irqsave(&host1x_syncpts_lock, flags);

	syncpt = idr_find(&host->syncpts, id);
	if (syncpt)
		snprintf(user_name, ARRAY_SIZE(user_name) - 1,
			 "%s", dev_name(syncpt->dev));

	spin_unlock_irqrestore(&host1x_syncpts_lock, flags);

	host1x_debug_output(o,
		"sync point %u hardware state: value %u, threshold %u, interrupt status %d, %s\n",
		id, value, thresh, status, syncpt ? user_name : "unused");
}

static void
host1x_soc_dump_syncpt(struct host1x_dbg_output *o,
		       struct host1x_syncpt *syncpt)
{
	host1x_soc_dump_syncpt_by_id(o, syncpt->host, syncpt->id);
}

static void
host1x_soc_dump_syncpts(struct host1x_dbg_output *o, struct host1x *host)
{
	unsigned int i;

	for (i = 0; i < HOST1X_SYNCPTS_NUM; i++)
		host1x_soc_dump_syncpt_by_id(o, host, i);
}
