/*
 * Copyright (C) 2016 NVIDIA CORPORATION.  All rights reserved.
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

#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "dev.h"
#include "fence.h"

static const char *host1x_fence_get_driver_name(struct dma_fence *fence)
{
	return "tegra-host1x";
}

static const char *host1x_fence_get_timeline_name(struct dma_fence *fence)
{
	return to_host1x_fence(fence)->name;
}

static bool host1x_fence_enable_signaling(struct dma_fence *fence)
{
	/*
	 * Syncpoint interrupt would happen even if syncpoint is already
	 * expired and hence fence would signal too. Since we are using
	 * syncpoints spinlock for the fence and syncpoint arming happens
	 * after fence creation, signaling is always enabled in our case.
	 */
	return true;
}

static void host1x_fence_release(struct dma_fence *fence)
{
	struct host1x_fence *f = to_host1x_fence(fence);

	module_put(f->sp->host->dev->driver->owner);
	kfree(f->name);
	kfree(f);
}

static const struct dma_fence_ops host1x_fence_ops = {
	.get_driver_name = host1x_fence_get_driver_name,
	.get_timeline_name = host1x_fence_get_timeline_name,
	.enable_signaling = host1x_fence_enable_signaling,
	.wait = dma_fence_default_wait,
	.release = host1x_fence_release,
};

struct dma_fence *host1x_fence_create(struct host1x_syncpt *sp, u32 threshold,
				      u64 context, u64 seqno)
{
	struct host1x_waitlist *waiter;
	struct host1x_fence *f;
	struct dma_fence *fence;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;

	f->thresh = threshold;
	f->name = kstrdup(sp->name, GFP_KERNEL);
	f->sp = sp;

	dma_fence_init(&f->base, &host1x_fence_ops, &sp->intr.lock,
		       context, seqno);

	/*
	 * Keep fence alive for the case where syncpoint signals
	 * earlier than fence got attached or BO get released before
	 * signaling occurs, signal handler would put fence to balance
	 * the reference counter.
	 */
	fence = dma_fence_get(&f->base);

	/*
	 * Avoid kernel module unloading while fence is alive because
	 * host1x must be available due to a use of syncpoints spinlock.
	 */
	__module_get(sp->host->dev->driver->owner);

	if (!host1x_syncpt_is_expired(sp, threshold)) {
		waiter = kzalloc(sizeof(*waiter), GFP_KERNEL);
		if (!waiter)
			goto err_free_fence;

		host1x_intr_add_action(sp->host, sp->id, threshold,
				       HOST1X_INTR_ACTION_SIGNAL_FENCE,
				       fence, waiter, NULL);
	} else {
		/*
		 * No need to arm an interrupt if syncpoint is already
		 * expired.
		 */
		dma_fence_signal(fence);
		dma_fence_put(fence);
	}

	return fence;

err_free_fence:
	kfree(f);

	return NULL;
}
EXPORT_SYMBOL(host1x_fence_create);

/**
 * host1x_fence_is_waitable() - Check if DMA fence can be waited by hardware
 * @fence: DMA fence
 *
 * Check is @fence is only backed by Host1x syncpoints and can therefore be
 * waited using only hardware.
 */
bool host1x_fence_is_waitable(struct dma_fence *fence)
{
	struct dma_fence_array *array;
	int i;

	array = to_dma_fence_array(fence);
	if (!array)
		return fence->ops == &host1x_fence_ops;

	for (i = 0; i < array->num_fences; ++i) {
		if (array->fences[i]->ops != &host1x_fence_ops)
			return false;
	}

	return true;
}
EXPORT_SYMBOL(host1x_fence_is_waitable);
