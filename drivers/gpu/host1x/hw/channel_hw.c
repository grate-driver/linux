/*
 * Tegra host1x Channel
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

#include <linux/dma-fence-array.h>
#include <linux/host1x.h>
#include <linux/slab.h>

#include <trace/events/host1x.h>

#include "../channel.h"
#include "../dev.h"
#include "../intr.h"
#include "../job.h"
#include "../fence.h"

#define HOST1X_CHANNEL_SIZE 16384
#define TRACE_MAX_LENGTH 128U

static void trace_write_gather(struct host1x_cdma *cdma, struct host1x_bo *bo,
			       u32 offset, u32 words)
{
	struct device *dev = cdma_to_channel(cdma)->dev;
	void *mem = NULL;

	if (host1x_debug_trace_cmdbuf)
		mem = host1x_bo_mmap(bo);

	if (mem) {
		u32 i;
		/*
		 * Write in batches of 128 as there seems to be a limit
		 * of how much you can output to ftrace at once.
		 */
		for (i = 0; i < words; i += TRACE_MAX_LENGTH) {
			u32 num_words = min(words - i, TRACE_MAX_LENGTH);

			offset += i * sizeof(u32);

			trace_host1x_cdma_push_gather(dev_name(dev), bo,
						      num_words, offset,
						      mem);
		}

		host1x_bo_munmap(bo, mem);
	}
}

static int prepend_waitchks(struct host1x_job *job,
			    unsigned int *wait_index,
			    unsigned int gather_index,
			    unsigned int *class)
{
	struct host1x_cdma *cdma = &job->channel->cdma;
	struct host1x *host = cdma_to_host1x(cdma);
	struct host1x_syncpt *waitchk_sp;
	struct host1x_waitchk *waitchk;
	unsigned int index = *wait_index;
	int err;

next_wait:
	if (index >= job->num_waitchks)
		goto out;

	waitchk = &job->waitchks[index];
	waitchk_sp = host->syncpts + waitchk->syncpt_id;

	if (waitchk->gather_index != gather_index)
		goto out;

	if (waitchk->relative)
		err = host1x_cdma_push(cdma,
			host1x_opcode_setclass(HOST1X_CLASS_HOST1X,
				host1x_uclass_wait_syncpt_base_r(), 1),
			host1x_class_host_wait_syncpt_base(waitchk->syncpt_id,
							   waitchk_sp->base->id,
							   waitchk->thresh));
	else
		err = host1x_cdma_push(cdma,
			host1x_opcode_setclass(HOST1X_CLASS_HOST1X,
				host1x_uclass_wait_syncpt_r(), 1),
			host1x_class_host_wait_syncpt(waitchk->syncpt_id,
						      waitchk->thresh));
	if (err)
		return err;

	*class = HOST1X_CLASS_HOST1X;
	index++;

	goto next_wait;
out:
	*wait_index = index;

	return 0;
}

static int submit_gathers(struct host1x_job *job)
{
	struct host1x_cdma *cdma = &job->channel->cdma;
	unsigned int class = 0;
	unsigned int i, k;
	int err;

	for (i = 0, k = 0; i < job->num_gathers; i++) {
		struct host1x_job_gather *g = &job->gathers[i];
		u32 op1 = host1x_opcode_gather(g->words);
		u32 op2 = g->base + g->offset;

		err = prepend_waitchks(job, &k, i, &class);
		if (err)
			return err;

		if (class != g->class) {
			err = host1x_cdma_push(cdma,
					host1x_opcode_setclass(g->class, 0, 0),
					HOST1X_OPCODE_NOP);
			if (err)
				return err;
		}

		trace_write_gather(cdma, g->bo, g->offset, op1 & 0xffff);

		err = host1x_cdma_push(cdma, op1, op2);
		if (err)
			return err;

		class = g->class;
	}

	return 0;
}

static int channel_push_fence(struct host1x_channel *ch,
			      struct dma_fence *fence)
{
	struct host1x_fence *f = to_host1x_fence(fence);
	u32 thresh = f->thresh;
	u32 id = f->sp->id;

	if (dma_fence_is_signaled(fence))
		return 0;

	return host1x_cdma_push(&ch->cdma,
				host1x_opcode_setclass(HOST1X_CLASS_HOST1X,
					host1x_uclass_wait_syncpt_r(), 1),
				host1x_class_host_wait_syncpt(id, thresh));
}

static int push_fences(struct host1x_channel *ch, struct host1x_job *job)
{
	struct dma_fence_array *array;
	struct dma_fence *fence;
	unsigned int i, k;
	int err;

	for (i = 0; i < job->num_fences; i++) {
		fence = job->fences[i];
		array = to_dma_fence_array(fence);
		if (!array) {
			err = channel_push_fence(ch, fence);
			if (err)
				return err;

			continue;
		}

		for (k = 0; k < array->num_fences; k++) {
			err = channel_push_fence(ch, array->fences[k]);
			if (err)
				return err;
		}
	}

	return 0;
}

static int synchronize_syncpt_base(struct host1x_job *job)
{
	struct host1x_syncpt *sp = job->syncpt;
	unsigned int id;
	u32 value;

	value = host1x_syncpt_read_max(sp);
	id = sp->base->id;

	return host1x_cdma_push(&job->channel->cdma,
			host1x_opcode_setclass(HOST1X_CLASS_HOST1X,
					HOST1X_UCLASS_LOAD_SYNCPT_BASE, 1),
			HOST1X_UCLASS_LOAD_SYNCPT_BASE_BASE_INDX_F(id) |
			HOST1X_UCLASS_LOAD_SYNCPT_BASE_VALUE_F(value));
}

static int channel_submit(struct host1x_job *job)
{
	struct host1x_channel *ch = job->channel;
	struct host1x *host = cdma_to_host1x(&ch->cdma);
	struct host1x_waitlist *completed_waiter;
	struct host1x_syncpt *sp;
	u32 user_syncpt_incrs = job->syncpt_incrs;
	u32 prev_max = 0;
	u32 syncval;
	int err;

	sp = job->syncpt;
	trace_host1x_channel_submit(dev_name(ch->dev),
				    job->num_gathers, job->num_relocs,
				    job->num_waitchks, sp->id,
				    job->syncpt_incrs);

	/* before error checks, return current max */
	prev_max = job->syncpt_end = host1x_syncpt_read_max(sp);

	/* get submit lock */
	err = mutex_lock_interruptible(&ch->submitlock);
	if (err)
		return err;

	completed_waiter = kzalloc(sizeof(*completed_waiter), GFP_KERNEL);
	if (!completed_waiter) {
		err = -ENOMEM;
		goto err_unlock;
	}

	/* begin a CDMA submit */
	err = host1x_cdma_begin(&ch->cdma, job);
	if (err)
		goto err_unlock;

	if (job->serialize) {
		/*
		 * Force serialization by inserting a host wait for the
		 * previous job to finish before this one can commence.
		 */
		err = host1x_cdma_push(&ch->cdma,
				host1x_opcode_setclass(HOST1X_CLASS_HOST1X,
					host1x_uclass_wait_syncpt_r(), 1),
				host1x_class_host_wait_syncpt(sp->id,
					host1x_syncpt_read_max(sp)));
		if (err)
			goto err_reset;
	}

	err = push_fences(ch, job);
	if (err)
		goto err_reset;

	/* Synchronize base register to allow using it for relative waiting */
	if (sp->base) {
		err = synchronize_syncpt_base(job);
		if (err)
			goto err_reset;
	}

	syncval = host1x_syncpt_incr_max(sp, user_syncpt_incrs + 1);

	host1x_hw_firewall_syncpt_assign_to_channel(host, sp, ch);

	job->syncpt_end = syncval;

	err = submit_gathers(job);
	if (err)
		goto err_reset;

	/*
	 * Append job with a syncpoint increment, ensuring that all
	 * outstanding operations are indeed completed before next job
	 * kicks in, otherwise jobs serialization isn't guaranteed.
	 */
	err = host1x_cdma_push(&ch->cdma,
			       host1x_opcode_nonincr(
					host1x_uclass_incr_syncpt_r(), 1),
			       host1x_uclass_incr_syncpt_cond_f(0x1) |
			       host1x_uclass_incr_syncpt_indx_f(sp->id));
	if (err)
		goto err_reset;

	/* end CDMA submit & stash pinned hMems into sync queue */
	host1x_cdma_end(&ch->cdma, job);

	trace_host1x_channel_submitted(dev_name(ch->dev), prev_max, syncval);

	/* schedule a submit complete interrupt */
	host1x_intr_add_action(host, sp->id, syncval,
			       HOST1X_INTR_ACTION_SUBMIT_COMPLETE, ch,
			       completed_waiter, NULL);

	mutex_unlock(&ch->submitlock);

	return 0;

err_reset:
	/*
	 * Job could be partially executed, reset HW and synchronize
	 * syncpoint to get into determined state.
	 */
	host1x_cdma_reset_locked(&ch->cdma, job->client);
	host1x_syncpt_sync(sp);

	/* CDMA was locked by host1x_cdma_begin() */
	mutex_unlock(&ch->cdma.lock);

err_unlock:
	mutex_unlock(&ch->submitlock);
	kfree(completed_waiter);

	return err;
}

static int host1x_channel_init(struct host1x_channel *ch, struct host1x *dev,
			       unsigned int index)
{
	ch->regs = dev->regs + index * HOST1X_CHANNEL_SIZE;
	return 0;
}

static const struct host1x_channel_ops host1x_channel_ops = {
	.init = host1x_channel_init,
	.submit = channel_submit,
};
