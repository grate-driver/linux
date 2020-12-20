/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __TEGRA_DRM_JOB_H
#define __TEGRA_DRM_JOB_H

#include <linux/bitmap.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

#include "drm.h"
/* include hw specification, host1x01 is common enough */
#include "host1x01_hardware.h"

struct tegra_drm_bo_fences {
	struct dma_fence *excl;
	struct dma_fence **shared;
	unsigned int num_shared;
};

struct tegra_drm_job {
	DECLARE_BITMAP(bos_write_bitmap, DRM_TEGRA_BO_TABLE_MAX_ENTRIES_NUM);
	DECLARE_BITMAP(bos_gart_bitmap,  DRM_TEGRA_BO_TABLE_MAX_ENTRIES_NUM);
	struct drm_sched_job sched_job;
	struct host1x *host;
	struct host1x_job base;
	struct tegra_drm *tegra;
	struct tegra_drm_channel *drm_channel;
	struct dma_fence *in_fence;
	struct dma_fence *hw_fence;
	struct drm_syncobj *out_syncobj;
	struct tegra_drm_bo_fences *bo_fences;
	struct tegra_bo **bos;
	unsigned int num_bos;
	struct kref refcount;
	bool prepared;
	u64 pipes;

	atomic_t *num_active_jobs;
	struct work_struct free_work;
	char task_name[TASK_COMM_LEN + 32];
};

static inline void
tegra_drm_init_job(struct tegra_drm_job *job,
		   struct tegra_drm *tegra,
		   struct drm_syncobj *out_syncobj,
		   struct dma_fence *in_fence,
		   struct host1x_syncpt *syncpt,
		   u64 fence_context,
		   atomic_t *num_active_jobs,
		   void (*free_work_func)(struct work_struct *work))
{
	char task_name[TASK_COMM_LEN];

	memset(job, 0, sizeof(*job));

	job->host		= syncpt->host;
	job->tegra		= tegra;
	job->out_syncobj	= out_syncobj;
	job->in_fence		= in_fence;
	job->num_active_jobs	= num_active_jobs;

	INIT_WORK(&job->free_work, free_work_func);
	host1x_init_job(&job->base, syncpt, fence_context);
	get_task_comm(task_name, current);
	snprintf(job->task_name, ARRAY_SIZE(job->task_name),
		 "process:%s pid:%d", task_name, current->pid);
	atomic_inc(num_active_jobs);
	kref_init(&job->refcount);
}

static inline void
tegra_drm_free_job(struct tegra_drm_job *job)
{
	host1x_finish_job(&job->base);
	schedule_work(&job->free_work);
}

static inline struct tegra_drm_job *
tegra_drm_job_get(struct tegra_drm_job *job)
{
	kref_get(&job->refcount);

	return job;
}

static inline void
tegra_drm_job_release(struct kref *kref)
{
	struct tegra_drm_job *job = container_of(kref, struct tegra_drm_job,
						 refcount);
	tegra_drm_free_job(job);
}

static inline void
tegra_drm_job_put(struct tegra_drm_job *job)
{
	kref_put(&job->refcount, tegra_drm_job_release);
}

int tegra_drm_copy_and_patch_cmdstream(const struct tegra_drm *tegra,
				       struct tegra_drm_job *drm_job,
				       struct tegra_bo *const *bos,
				       u64 pipes_expected,
				       u32 *words_in,
				       u64 *ret_pipes,
				       unsigned int *ret_incrs);

int tegra_drm_submit_job_v1(struct drm_device *drm,
			    struct drm_tegra_submit *submit,
			    struct drm_file *file);

int tegra_drm_submit_job_v2(struct drm_device *drm,
			    struct drm_tegra_submit_v2 *submit,
			    struct drm_file *file);

#endif
