/*
 * Tegra host1x Command DMA
 *
 * Copyright (c) 2010-2013, NVIDIA Corporation.
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


#include <asm/cacheflush.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/host1x.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/slab.h>
#include <trace/events/host1x.h>

#include "cdma.h"
#include "channel.h"
#include "dev.h"
#include "debug.h"
#include "job.h"

/*
 * push_buffer
 *
 * The push buffer is a circular array of words to be fetched by command DMA.
 * Note that it works slightly differently to the sync queue; fence == pos
 * means that the push buffer is full, not empty.
 */

#define HOST1X_PUSHBUFFER_SLOTS		512
#define HOST1X_PUSHBUFFER_FLUSH_PERIOD	8

/*
 * Clean up push buffer resources
 */
static void host1x_pushbuffer_destroy(struct push_buffer *pb)
{
	struct host1x_cdma *cdma = pb_to_cdma(pb);
	struct host1x *host1x = cdma_to_host1x(cdma);

	if (!pb->phys)
		return;

	if (host1x->domain) {
		iommu_unmap(host1x->domain, pb->dma, pb->alloc_size);
		free_iova(&host1x->iova, iova_pfn(&host1x->iova, pb->dma));
	}

	dma_free_wc(host1x->dev, pb->alloc_size, pb->mapped, pb->phys);

	pb->mapped = NULL;
	pb->phys = 0;
}

/*
 * Init push buffer resources
 */
static int host1x_pushbuffer_init(struct push_buffer *pb)
{
	struct host1x_cdma *cdma = pb_to_cdma(pb);
	struct host1x *host1x = cdma_to_host1x(cdma);
	struct iova *alloc;
	u32 size;
	int err;

	pb->mapped = NULL;
	pb->phys = 0;
	pb->size = HOST1X_PUSHBUFFER_SLOTS * 8;

	size = pb->size + 4;

	/* initialize buffer pointers */
	pb->fence = pb->size - 8;
	pb->pos = 0;

	if (host1x->domain) {
		unsigned long shift;

		size = iova_align(&host1x->iova, size);

		pb->mapped = dma_alloc_wc(host1x->dev, size, &pb->phys,
					  GFP_KERNEL);
		if (!pb->mapped)
			return -ENOMEM;

		shift = iova_shift(&host1x->iova);
		alloc = alloc_iova(&host1x->iova, size >> shift,
				   host1x->iova_end >> shift, true);
		if (!alloc) {
			err = -ENOMEM;
			goto iommu_free_mem;
		}

		pb->dma = iova_dma_addr(&host1x->iova, alloc);
		err = iommu_map(host1x->domain, pb->dma, pb->phys, size,
				IOMMU_READ);
		if (err)
			goto iommu_free_iova;
	} else {
		pb->mapped = dma_alloc_wc(host1x->dev, size, &pb->phys,
					  GFP_KERNEL);
		if (!pb->mapped)
			return -ENOMEM;

		pb->dma = pb->phys;
	}

	pb->alloc_size = size;

	host1x_hw_pushbuffer_init(host1x, pb);

	return 0;

iommu_free_iova:
	__free_iova(&host1x->iova, alloc);
iommu_free_mem:
	dma_free_wc(host1x->dev, pb->alloc_size, pb->mapped, pb->phys);

	return err;
}

/*
 * Push two words to the push buffer
 * Caller must ensure push buffer is not full
 */
static void host1x_pushbuffer_push(struct push_buffer *pb, u32 op1, u32 op2)
{
	u32 *p = (u32 *)((void *)pb->mapped + pb->pos);

	WARN_ON(pb->pos == pb->fence);
	*(p++) = op1;
	*(p++) = op2;
	pb->pos = (pb->pos + 8) & (pb->size - 1);
}

/*
 * Pop a number of two word slots from the push buffer
 * Caller must ensure push buffer is not empty
 */
static void host1x_pushbuffer_pop(struct push_buffer *pb, unsigned int slots)
{
	/* Advance the next write position */
	pb->fence = (pb->fence + slots * 8) & (pb->size - 1);
}

static u32 pushbuffer_space(u32 pos, u32 fence, u32 size)
{
	/* Note that position is adjusted by 8 to avoid DMA GET == PUT */
	return ((fence - (pos + 8)) & (size - 1)) / 8;
}

/*
 * Return the number of two word slots free in the push buffer
 */
static u32 host1x_pushbuffer_space(struct push_buffer *pb)
{
	return pushbuffer_space(pb->pos, pb->fence, pb->size);
}

/*
 * Pop the completed pushes and return true if DMA fetching progressed.
 * Otherwise return false.
 */
static bool host1x_pushbuffer_cdma_progressed(struct host1x *host1x,
					      struct host1x_cdma *cdma,
					      struct push_buffer *pb)
{
	u32 pos, space;

	pos = host1x_hw_cdma_position(host1x, cdma);
	space = pushbuffer_space(pb->fence, pos, pb->size);

	if (space) {
		host1x_pushbuffer_pop(pb, space);
		cdma->first_get = pb->fence;
		cdma->slots_used -= space;
	}

	return (space > 0);
}

/*
 * Wait for DMA to progress and pushbuffer to gain some free space
 * until timeout expired.
 */
static int host1x_pushbuffer_space_wait(struct host1x_cdma *cdma,
					struct push_buffer *pb)
{
	struct host1x *host1x = cdma_to_host1x(cdma);
	unsigned int tries = 30;
	unsigned int i = 1;

	do {
		if (host1x_pushbuffer_cdma_progressed(host1x, cdma, pb))
			break;

		udelay(3 * i++);
	} while (--tries);

	if (!tries) {
		dev_err(host1x->dev, "Timeout waiting channel %u to progress\n",
			cdma->prepared_job->channel->id);
		return -ETIMEDOUT;
	}

	return 0;
}

/*
 * Sleep (if necessary) until the requested event happens
 *   - CDMA_EVENT_SYNC_QUEUE_EMPTY : sync queue is completely empty.
 *     - Returns 1
 *   - CDMA_EVENT_PUSH_BUFFER_SPACE : there is space in the push buffer
 *     - Return the amount of space (> 0)
 * Must be called with the cdma lock held.
 */
int host1x_cdma_wait_locked(struct host1x_cdma *cdma, enum cdma_event event,
			    bool interruptible)
{
	struct push_buffer *pb = &cdma->push_buffer;
	int space, ret = 0;

	do {
		bool idle = list_empty(&cdma->sync_queue);

		switch (event) {
		case CDMA_EVENT_SYNC_QUEUE_EMPTY:
			space = idle ? 1 : 0;
			break;

		case CDMA_EVENT_PUSH_BUFFER_SPACE:
			space = host1x_pushbuffer_space(pb);

			/*
			 * Semaphore below will lockup channel if CMDA
			 * is idling. Assuming that DMA would progress,
			 * try to poll-update DMA status and error out if
			 * DMA got stuck for a substantial time.
			 */
			if (!space && idle) {
				ret = host1x_pushbuffer_space_wait(cdma, pb);
				if (ret)
					return ret;

				space = host1x_pushbuffer_space(pb);
			}
			break;

		default:
			unreachable();
		}

		if (space)
			return space;

		trace_host1x_wait_cdma(dev_name(cdma_to_channel(cdma)->dev),
				       event);

		/* If somebody has managed to already start waiting, yield */
		if (cdma->event != CDMA_EVENT_NONE) {
			mutex_unlock(&cdma->lock);
			schedule();
			mutex_lock(&cdma->lock);
			continue;
		}

		cdma->event = event;

		mutex_unlock(&cdma->lock);

		if (interruptible)
			ret = down_interruptible(&cdma->sem);
		else
			down(&cdma->sem);

		mutex_lock(&cdma->lock);
	} while (ret == 0);

	return ret;
}

/*
 * Start timer that tracks the time spent by the job.
 * Must be called with the cdma lock held.
 */
static void cdma_start_timer_locked(struct host1x_cdma *cdma,
				    struct host1x_job *job)
{
	if (cdma->timeout.client) {
		/* timer already started */
		return;
	}

	cdma->timeout.client = job->client;
	cdma->timeout.syncpt = job->syncpt;
	cdma->timeout.syncpt_val = job->syncpt_end;
	cdma->timeout.start_ktime = ktime_get();

	schedule_delayed_work(&cdma->timeout.wq,
			      msecs_to_jiffies(job->timeout));
}

/*
 * Stop timer when a buffer submission completes.
 * Must be called with the cdma lock held.
 */
static void stop_cdma_timer_locked(struct host1x_cdma *cdma)
{
	cancel_delayed_work(&cdma->timeout.wq);
	cdma->timeout.client = NULL;
}

/*
 * For all sync queue entries that have already finished according to the
 * current sync point registers:
 *  - unpin & unref their mems
 *  - pop their push buffer slots
 *  - remove them from the sync queue
 * This is normally called from the host code's worker thread, but can be
 * called manually if necessary.
 * Must be called with the cdma lock held.
 */
static void update_cdma_locked(struct host1x_cdma *cdma)
{
	struct host1x_job *job, *n;
	bool signal = false;

	/* If CDMA is stopped, queue is cleared and we can return */
	if (!cdma->running)
		return;

	/*
	 * Walk the sync queue, reading the sync point registers as necessary,
	 * to consume as many sync queue entries as possible without blocking
	 */
	list_for_each_entry_safe(job, n, &cdma->sync_queue, list) {
		struct host1x_syncpt *sp = job->syncpt;

		/* Check whether this syncpt has completed, and bail if not */
		if (!host1x_syncpt_is_expired(sp, job->syncpt_end)) {
			/* Start timer on next pending syncpt */
			if (job->timeout)
				cdma_start_timer_locked(cdma, job);

			break;
		}

		/* Cancel timeout, when a buffer completes */
		if (cdma->timeout.client)
			stop_cdma_timer_locked(cdma);

		/* Unpin the memory */
		host1x_job_unpin(job);

		/* Pop push buffer slots */
		if (job->num_slots) {
			struct push_buffer *pb = &cdma->push_buffer;

			host1x_pushbuffer_pop(pb, job->num_slots);

			if (cdma->event == CDMA_EVENT_PUSH_BUFFER_SPACE)
				signal = true;
		}

		list_del(&job->list);
		host1x_job_put(job);
	}

	if (cdma->event == CDMA_EVENT_SYNC_QUEUE_EMPTY &&
	    list_empty(&cdma->sync_queue))
		signal = true;

	if (signal) {
		cdma->event = CDMA_EVENT_NONE;
		up(&cdma->sem);
	}
}

void host1x_cdma_update_sync_queue(struct host1x_cdma *cdma,
				   struct device *dev)
{
	struct host1x *host1x = cdma_to_host1x(cdma);
	struct host1x_job *job;
	u32 restart_addr, syncpt_incrs, syncpt_val;

	job = list_first_entry(&cdma->sync_queue, struct host1x_job, list);

	syncpt_val = host1x_syncpt_load(job->syncpt);

	dev_dbg(dev, "%s: starting cleanup (thresh %d)\n",
		__func__, syncpt_val);

	/*
	 * Move the sync_queue read pointer to the first entry that hasn't
	 * completed based on the current HW syncpt value. It's likely there
	 * won't be any (i.e. we're still at the head), but covers the case
	 * where a syncpt incr happens just prior/during the teardown.
	 */

	dev_dbg(dev, "%s: skip completed buffers still in sync_queue\n",
		__func__);

	host1x_job_dump(dev, job);

	/*
	 * Walk the sync_queue, first incrementing with the CPU syncpts that
	 * are partially executed (the first buffer) or fully skipped while
	 * still in the current context (slots are also NOP-ed).
	 *
	 * At the point contexts are interleaved, syncpt increments must be
	 * done inline with the pushbuffer from a GATHER buffer to maintain
	 * the order (slots are modified to be a GATHER of syncpt incrs).
	 *
	 * Note: save in restart_addr the location where the timed out buffer
	 * started in the PB, so we can start the refetch from there (with the
	 * modified NOP-ed PB slots). This lets things appear to have completed
	 * properly for this buffer and resources are freed.
	 */

	dev_dbg(dev, "%s: perform CPU incr on pending same ctx buffers\n",
		__func__);

	/* won't need a timeout when replayed */
	job->timeout = 0;

	/* do the remaining jobs syncpoint increments */
	syncpt_incrs = job->syncpt_end - syncpt_val;
	dev_dbg(dev, "%s: CPU incr (%d)\n", __func__, syncpt_incrs);

	/* safe to use CPU to incr syncpts */
	host1x_hw_cdma_timeout_cpu_incr(host1x, cdma, job->first_get,
					syncpt_incrs, job->syncpt_end,
					job->num_slots);

	dev_dbg(dev, "%s: finished sync_queue modification\n", __func__);

	if (!list_empty(&cdma->sync_queue))
		restart_addr = job->first_get;
	else
		restart_addr = cdma->last_pos;

	/* roll back DMAGET and start up channel again */
	host1x_hw_cdma_resume(host1x, cdma);
	host1x_hw_cdma_restart(host1x, cdma, restart_addr);
}

/*
 * Create a cdma
 */
int host1x_cdma_init(struct host1x_cdma *cdma)
{
	int err;

	mutex_init(&cdma->lock);
	sema_init(&cdma->sem, 0);

	INIT_LIST_HEAD(&cdma->sync_queue);

	cdma->event = CDMA_EVENT_NONE;
	cdma->running = false;
	cdma->torndown = false;

	err = host1x_pushbuffer_init(&cdma->push_buffer);
	if (err)
		return err;

	return 0;
}

/*
 * Destroy a cdma
 */
int host1x_cdma_deinit(struct host1x_cdma *cdma)
{
	struct push_buffer *pb = &cdma->push_buffer;
	struct host1x *host1x = cdma_to_host1x(cdma);

	if (cdma->running) {
		pr_warn("%s: CDMA still running\n", __func__);
		return -EBUSY;
	}

	host1x_pushbuffer_destroy(pb);
	host1x_hw_cdma_timeout_destroy(host1x, cdma);

	return 0;
}

/*
 * Begin a cdma submit
 */
int host1x_cdma_begin(struct host1x_cdma *cdma, struct host1x_job *job)
{
	struct host1x *host1x = cdma_to_host1x(cdma);

	mutex_lock(&cdma->lock);

	if (job->timeout) {
		/* init state on first submit with timeout value */
		if (!cdma->timeout.initialized) {
			int err;

			err = host1x_hw_cdma_timeout_init(host1x, cdma,
							  job->syncpt->id);
			if (err) {
				mutex_unlock(&cdma->lock);
				return err;
			}
		}
	}

	if (!cdma->running)
		host1x_hw_cdma_start(host1x, cdma);

	cdma->prepared_job = job;
	cdma->slots_used = 0;
	cdma->first_get = cdma->push_buffer.pos;

	trace_host1x_cdma_begin(dev_name(job->channel->dev));
	return 0;
}

/*
 * Push two words into a push buffer slot
 * Blocks as necessary if the push buffer is full.
 */
int host1x_cdma_push(struct host1x_cdma *cdma, u32 op1, u32 op2)
{
	struct host1x *host1x = cdma_to_host1x(cdma);
	struct push_buffer *pb = &cdma->push_buffer;
	int err;

	if (host1x_debug_trace_cmdbuf)
		trace_host1x_cdma_push(dev_name(cdma_to_channel(cdma)->dev),
				       op1, op2);

	/* flush pushbuffer periodically to achieve better performance */
	if (cdma->slots_used % HOST1X_PUSHBUFFER_FLUSH_PERIOD == 0)
		host1x_hw_cdma_flush(host1x, cdma);

	if (host1x_pushbuffer_space(pb) == 0) {
		host1x_hw_cdma_flush(host1x, cdma);

		err = host1x_cdma_wait_locked(
				cdma, CDMA_EVENT_PUSH_BUFFER_SPACE, true);
		if (err < 0)
			return err;
	}

	cdma->slots_used++;
	host1x_pushbuffer_push(pb, op1, op2);

	return 0;
}

/*
 * End a cdma submit
 * Kick off DMA, add job to the sync queue, and a number of slots to be freed
 * from the pushbuffer. The handles for a submit must all be pinned at the same
 * time, but they can be unpinned in smaller chunks.
 */
void host1x_cdma_end(struct host1x_cdma *cdma,
		     struct host1x_job *job)
{
	struct host1x *host1x = cdma_to_host1x(cdma);
	bool idle = list_empty(&cdma->sync_queue);

	host1x_hw_cdma_flush(host1x, cdma);

	cdma->prepared_job = NULL;

	job->first_get = cdma->first_get;
	job->num_slots = cdma->slots_used;
	host1x_job_get(job);
	list_add_tail(&job->list, &cdma->sync_queue);

	/* start timer on idle -> active transitions */
	if (job->timeout && idle)
		cdma_start_timer_locked(cdma, job);

	trace_host1x_cdma_end(dev_name(job->channel->dev));
	mutex_unlock(&cdma->lock);
}

/*
 * End a cdma submit
 * Stop CDMA abd cancel the current in-progress job, cleaning CDMA state
 */
void host1x_cdma_end_abort(struct host1x_cdma *cdma,
			   struct host1x_job *job)
{
	/*
	 * Job could be partially executed, reset HW and synchronize
	 * syncpoint to get into determined state.
	 */
	host1x_cdma_reset_locked(cdma, job->client);
	host1x_syncpt_sync(job->syncpt);

	cdma->prepared_job = NULL;

	mutex_unlock(&cdma->lock);
}

/*
 * Update cdma state according to current sync point values
 */
void host1x_cdma_update(struct host1x_cdma *cdma)
{
	mutex_lock(&cdma->lock);
	update_cdma_locked(cdma);
	mutex_unlock(&cdma->lock);
}

/*
 * Reset channels state and the clients module
 */
void host1x_cdma_reset_locked(struct host1x_cdma *cdma,
			      struct host1x_client *client)
{
	struct host1x *host = cdma_to_host1x(cdma);

	/*
	 * There is no need to reset channel if CDMA hasn't been flushed.
	 * In that case simply revert back pushbufs pointer to the initial
	 * state.
	 */
	if (cdma->last_pos == cdma->first_get) {
		cdma->push_buffer.pos = cdma->first_get;
		return;
	}

	/* wait for jobs completion, if any */
	host1x_cdma_wait_locked(cdma, CDMA_EVENT_SYNC_QUEUE_EMPTY, false);

	/* stop the channel and reset client */
	host1x_hw_cdma_freeze(host, cdma, client);

	/* skip partially executed pushes */
	host1x_pushbuffer_pop(&cdma->push_buffer, cdma->slots_used);

	/* re-enable commands processor, keeping DMA stopped */
	host1x_hw_cdma_resume(host, cdma);
}
