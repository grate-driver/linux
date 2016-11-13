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

struct host1x_fence {
	struct dma_fence base;
	struct module *module;
	const char *timeline_name;
};

static inline struct host1x_fence *to_host1x_fence(struct dma_fence *fence)
{
	return container_of(fence, struct host1x_fence, base);
}

static const char *host1x_fence_get_driver_name(struct dma_fence *fence)
{
	return "tegra-host1x";
}

static const char *host1x_fence_get_timeline_name(struct dma_fence *fence)
{
	struct host1x_fence *f = to_host1x_fence(fence);
	return f->timeline_name;
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
	module_put(f->module);
	kfree(f);
}

static const struct dma_fence_ops host1x_fence_ops = {
	.get_driver_name = host1x_fence_get_driver_name,
	.get_timeline_name = host1x_fence_get_timeline_name,
	.enable_signaling = host1x_fence_enable_signaling,
	.wait = dma_fence_default_wait,
	.release = host1x_fence_release,
};

struct dma_fence *host1x_fence_create(struct host1x_client *client,
				      struct host1x_syncpt *sp, u32 threshold)
{
	struct host1x_waitlist *waiter = NULL;
	struct host1x *host = sp->host;
	struct host1x_fence *f;
	struct dma_fence *fence;
	int err;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		goto error_cleanup;

	waiter = kzalloc(sizeof(*waiter), GFP_KERNEL);
	if (!waiter)
		goto error_cleanup;

	/* use syncpoints name for the fence */
	f->timeline_name = sp->name;

	dma_fence_init(&f->base, &host1x_fence_ops, &sp->intr.lock,
		       host1x_syncpt_get_fence_context(sp), threshold);
	/*
	 * Keep fence alive for the case where syncpoint signals earlier
	 * than fence got attached or BO get released before signaling occurs,
	 * signal handler would put fence to balance the reference counter.
	 */
	fence = dma_fence_get(&f->base);

	err = host1x_intr_add_action(host, sp->id, threshold,
				     HOST1X_INTR_ACTION_SIGNAL_FENCE,
				     fence, waiter, NULL);
	if (err)
		goto error_cleanup;

	/*
	 * Avoid kernel module unloading while fence is alive because
	 * backing syncpoint would be destroyed on host1x driver removal.
	 */
	f->module = client->dev->driver->owner;
	__module_get(f->module);

	return fence;

error_cleanup:
	kfree(waiter);
	kfree(f);

	return NULL;
}
EXPORT_SYMBOL(host1x_fence_create);
