/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/kthread.h>

#include "debug.h"
#include "job.h"
#include "scheduler.h"

static inline struct tegra_drm_job *
to_tegra_drm_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct tegra_drm_job, sched_job);
}

static inline bool
no_need_to_wait_for_fence(struct dma_fence *f, struct host1x_channel *channel)
{
	struct host1x_fence *fence = to_host1x_fence(f);

	/* NULL if it's not a host1x's fence */
	if (!fence)
		return false;

	if (fence->channel != channel)
		return false;

	/*
	 * There is no need to wait for the fence if it is host1x_fence and
	 * fence is on the same hardware channel as the job because jobs
	 * are naturally ordered in the channel's queue.
	 */

	return true;
}

static struct dma_fence *
tegra_drm_sched_dependency(struct drm_sched_job *sched_job,
			   struct drm_sched_entity *entity)
{
	struct tegra_drm_job *job = to_tegra_drm_job(sched_job);
	struct tegra_drm_channel *drm_channel = job->drm_channel;
	struct host1x_channel *channel = drm_channel->channel;
	struct dma_fence *fence;
	unsigned int i, k;

	if (job->in_fence) {
		fence = job->in_fence;
		job->in_fence = NULL;

		if (!no_need_to_wait_for_fence(fence, channel) &&
		    !dma_fence_is_signaled(fence))
			return fence;

		dma_fence_put(fence);
	}

	if (!job->bo_fences)
		return NULL;

	for (i = 0; i < job->num_bos; i++) {
		struct tegra_drm_bo_fences *f = &job->bo_fences[i];

		if (f->excl) {
			fence = f->excl;
			f->excl = NULL;

			if (!no_need_to_wait_for_fence(fence, channel) &&
			    !dma_fence_is_signaled(fence))
				return fence;

			dma_fence_put(fence);
		}

		for (k = 0; k < f->num_shared; k++) {
			if (!f->shared[k])
				continue;

			fence = f->shared[k];
			f->shared[k] = NULL;

			if (!no_need_to_wait_for_fence(fence, channel) &&
			    !dma_fence_is_signaled(fence))
				return fence;

			dma_fence_put(fence);
		}

		f->num_shared = 0;
	}

	return NULL;
}

static struct dma_fence *
tegra_drm_sched_run_job(struct drm_sched_job *sched_job)
{
	struct tegra_drm_job *job = to_tegra_drm_job(sched_job);
	struct tegra_drm_channel *drm_channel = job->drm_channel;
	struct host1x_channel *channel = drm_channel->channel;
	struct dma_fence *fence;

	if (sched_job->s_fence->finished.error)
		return NULL;

	fence = host1x_channel_submit(channel, &job->base, job->hw_fence);

	if (job->out_syncobj)
		drm_syncobj_replace_fence(job->out_syncobj, fence);

	if (!job->hw_fence)
		job->hw_fence = dma_fence_get(fence);

	return fence;
}

static inline void tegra_drm_recover_hardware(struct tegra_drm_job *drm_job)
{
	struct tegra_drm_channel *drm_channel = drm_job->drm_channel;
	struct drm_gpu_scheduler *sched = drm_job->sched_job.sched;
	struct host1x_channel *channel = drm_channel->channel;
	struct host1x_syncpt *syncpt = drm_job->base.syncpt;
	struct tegra_drm *tegra = drm_job->tegra;
	struct host1x_job *job = &drm_job->base;
	struct tegra_drm_client *drm_client;
	u64 pipes = drm_job->pipes;

	DRM_ERROR("%s: pipes 0x%llx (%s)\n",
		  sched->name, pipes, drm_job->task_name);

	/* reset channel's HW, now the channel is idling */
	host1x_channel_reset(channel);

	/* detach all fences from sync point without signaling them */
	host1x_syncpt_detach_fences(syncpt);

	/* reset job's sync point state */
	host1x_syncpt_reset(syncpt, -ETIMEDOUT);

	/* unlock MLOCKs held by the channel */
	host1x_unlock_channel_mlocks(channel);

	/*
	 * Job could be completed due to timeout-check racing nature,
	 * although that's very unlikely to happen and more is indication
	 * of a bug somewhere.
	 */
	if (dma_fence_is_signaled(drm_job->hw_fence)) {
		DRM_INFO("%s: job happened to complete\n", sched->name);
		return;
	}

	/* fence shall not signal at this point */
	host1x_channel_cleanup_job(channel, job, drm_job->hw_fence);

	/*
	 * Reset client's HW. Note that technically this could reset
	 * active-and-good client in a case of multi-client channel (GR3D),
	 * but HW hang is an extreme case and hence it doesn't matter much
	 * if a good unrelated job will get aborted here as well.
	 */
	list_for_each_entry(drm_client, &tegra->clients, list) {
		if (!drm_client->reset_hw)
			continue;

		if (pipes & drm_client->pipe) {
			DRM_DEV_INFO(drm_client->base.dev, "resetting hw\n");
			drm_client->reset_hw(drm_client);
		}
	}

	/* this fence is done now */
	dma_fence_put(drm_job->hw_fence);
	drm_job->hw_fence = NULL;
}

static void tegra_drm_sched_timedout_job(struct drm_sched_job *sched_job)
{
	struct tegra_drm_job *job = to_tegra_drm_job(sched_job);
	struct drm_gpu_scheduler *sched = sched_job->sched;
	u64 pipes = job->pipes;

	DRM_WARN("%s: %s: pipes 0x%llx (%s)\n",
		 __func__, sched->name, pipes, job->task_name);

	/*
	 * drm_sched_main() queues job before running it, hence it may
	 * happen that timeout happens before job even had a chance to
	 * start and this actually happens in practice under load.
	 */
	if (!job->hw_fence) {
		DRM_WARN("%s: %s: job is inactive (%s)\n",
			 __func__, sched->name, job->task_name);
		return;
	}

	if (dma_fence_is_signaled(job->hw_fence))
		return;

	drm_sched_stop(sched, sched_job);
	drm_sched_increase_karma(sched_job);

	tegra_drm_debug_dump_hung_job(job);
	tegra_drm_recover_hardware(job);

	drm_sched_resubmit_jobs(sched);
	drm_sched_start(sched, true);
}

static void tegra_drm_sched_free_job(struct drm_sched_job *sched_job)
{
	struct tegra_drm_job *job = to_tegra_drm_job(sched_job);

	drm_sched_job_cleanup(sched_job);
	tegra_drm_job_put(job);
}

const struct drm_sched_backend_ops tegra_drm_sched_ops = {
	.timedout_job = tegra_drm_sched_timedout_job,
	.dependency = tegra_drm_sched_dependency,
	.free_job = tegra_drm_sched_free_job,
	.run_job = tegra_drm_sched_run_job,
};
