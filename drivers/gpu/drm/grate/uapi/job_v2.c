/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/bitops.h>

#include "debug.h"
#include "gart.h"
#include "job.h"

#define JOB_ERROR(fmt, args...) \
	DRM_ERROR_RATELIMITED(fmt " (%s)\n", ##args, job->task_name)

#define JOB_DEV_ERROR(dev, fmt, args...) \
	DRM_DEV_ERROR_RATELIMITED(dev, fmt " (%s)\n", ##args, job->task_name)

static inline struct tegra_bo **
tegra_drm_job_bos_ptr(struct tegra_drm_job *job)
{
	return (struct tegra_bo **) (job + 1);
}

static inline struct tegra_drm_bo_fences *
tegra_drm_job_bo_fences_ptr(struct tegra_drm_job *job,
			    struct drm_tegra_submit_v2 *submit)
{
	struct tegra_bo **bos = tegra_drm_job_bos_ptr(job);

	return (struct tegra_drm_bo_fences *) (bos + submit->num_bos);
}

static inline struct drm_tegra_bo_table_entry *
tegra_drm_user_data_bo_table_ptr(void *user_data)
{
	return user_data;
}

static inline u32 *
tegra_drm_user_data_cmdstream_ptr(void *user_data,
				  struct drm_tegra_submit_v2 *submit)
{
	struct drm_tegra_bo_table_entry *bo_table =
				tegra_drm_user_data_bo_table_ptr(user_data);

	return (u32*) (bo_table + submit->num_bos);
}

static inline int
tegra_drm_check_submit(struct drm_tegra_submit_v2 *submit)
{
	if (!submit->num_cmdstream_words ||
	     submit->num_cmdstream_words > 0xffffff) {
		DRM_ERROR_RATELIMITED("invalid num_cmdstream_words: %u\n",
				      submit->num_cmdstream_words);
		return -EINVAL;
	}

	if (submit->num_bos > DRM_TEGRA_BO_TABLE_MAX_ENTRIES_NUM) {
		DRM_ERROR_RATELIMITED("invalid num_bos: %u\n", submit->num_bos);
		return -EINVAL;
	}

	return 0;
}

static inline void tegra_drm_cleanup_job_fences(struct tegra_drm_job *job)
{
	unsigned int i, k;

	if (job->out_syncobj)
		drm_syncobj_put(job->out_syncobj);

	dma_fence_put(job->in_fence);
	dma_fence_put(job->hw_fence);

	if (!job->bo_fences)
		return;

	for (i = 0; i < job->num_bos; i++) {
		struct tegra_drm_bo_fences *f = &job->bo_fences[i];

		dma_fence_put(f->excl);

		for (k = 0; k < f->num_shared; k++)
			dma_fence_put(f->shared[k]);
	}
}

static inline int
tegra_drm_unprepare_job(struct tegra_drm_job *job)
{
	struct tegra_drm_client *drm_client;
	u64 pipes = job->pipes;
	int err;

	if (!job->prepared)
		return 0;

	list_for_each_entry(drm_client, &job->tegra->clients, list) {
		if (!drm_client->unprepare_job || !(pipes & drm_client->pipe))
			continue;

		err = drm_client->unprepare_job(drm_client, job);
		if (err)
			JOB_DEV_ERROR(drm_client->base.dev,
				      "failed to unprepare job: %d\n", err);
	}

	job->prepared = false;

	return 0;
}

static inline void tegra_drm_put_job_bos(struct tegra_drm_job *job)
{
	struct tegra_bo **job_bos = tegra_drm_job_bos_ptr(job);
	struct drm_gem_object *gem;
	unsigned int i;

	tegra_drm_job_unmap_gart(job, job_bos);

	for (i = 0; i < job->num_bos; i++) {
		gem = &job_bos[i]->gem;
		drm_gem_object_put(gem);
	}
}

static void tegra_drm_free_job_v2(struct work_struct *work)
{
	struct tegra_drm_job *job = container_of(work, struct tegra_drm_job,
						 free_work);
	atomic_t *num_active_jobs = job->num_active_jobs;

	host1x_syncpt_detach_fences(job->base.syncpt);
	host1x_cleanup_job(job->host, &job->base);
	tegra_drm_cleanup_job_fences(job);
	tegra_drm_unprepare_job(job);
	tegra_drm_put_job_bos(job);
	kfree(job);

	atomic_dec(num_active_jobs);
}

static inline int
tegra_drm_allocate_job(struct host1x *host,
		       struct drm_device *drm,
		       struct tegra_drm *tegra,
		       struct drm_tegra_submit_v2 *submit,
		       struct drm_file *file,
		       struct tegra_drm_job **ret_job,
		       void **ret_user_data)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct host1x_syncpt *syncpt;
	struct drm_syncobj *syncobj;
	struct tegra_drm_job *job;
	struct dma_fence *fence;
	void *user_data;
	size_t size;
	int err;

	syncpt = host1x_syncpt_request(host);
	if (IS_ERR(syncpt)) {
		err = PTR_ERR(syncpt);
		DRM_DEBUG("failed to request sync point: %d\n", err);
		return err;
	}

	size = sizeof(*job) +
	       sizeof(struct tegra_bo *) * submit->num_bos +
	       sizeof(struct tegra_drm_bo_fences) * submit->num_bos;

	job = kzalloc(size, GFP_NOWAIT);
	if (!job) {
		err = -ENOMEM;
		goto err_put_syncpt;
	}

	size = sizeof(u32) * (submit->num_cmdstream_words) +
	       sizeof(struct drm_tegra_bo_table_entry) * submit->num_bos;

	user_data = kzalloc(size, GFP_NOWAIT);
	if (!user_data) {
		err = -ENOMEM;
		goto err_free_job;
	}

	/* XXX: merge in_fence with out_fence? */
	if (submit->in_fence) {
		syncobj = drm_syncobj_find(file, submit->in_fence);
		if (!syncobj) {
			err = -ENOENT;
			goto err_free_data;
		}

		fence = drm_syncobj_fence_get(syncobj);
		drm_syncobj_put(syncobj);
	} else {
		fence = NULL;
	}

	if (submit->out_fence) {
		syncobj = drm_syncobj_find(file, submit->out_fence);
		if (!syncobj) {
			err = -ENOENT;
			goto err_free_data;
		}
	} else {
		syncobj = NULL;
	}

	tegra_drm_init_job(job, tegra, syncobj, fence, syncpt,
			   fpriv->drm_context, &fpriv->num_active_jobs,
			   tegra_drm_free_job_v2);

	*ret_user_data = user_data;
	*ret_job = job;

	return 0;

err_free_data:
	kfree(user_data);

err_free_job:
	kfree(job);

err_put_syncpt:
	host1x_syncpt_put(syncpt);

	return err;
}

static inline int
tegra_drm_copy_user_data(struct tegra_drm_job *job, void *user_data,
			 struct drm_tegra_submit_v2 *submit)
{
	struct drm_tegra_bo_table_entry *bo_table;
	struct drm_tegra_bo_table_entry __user *user_bo_table;
	struct u32 __user *user_cmdstream;
	u32 *cmdstream;
	size_t size;

	user_bo_table = u64_to_user_ptr(submit->bo_table_ptr);
	user_cmdstream = u64_to_user_ptr(submit->cmdstream_ptr);

	bo_table = tegra_drm_user_data_bo_table_ptr(user_data);
	cmdstream = tegra_drm_user_data_cmdstream_ptr(user_data, submit);

	size = sizeof(*bo_table) * submit->num_bos;
	if (size && copy_from_user(bo_table, user_bo_table, size)) {
		JOB_ERROR("failed to copy bo_table");
		return -EFAULT;
	}

	size = sizeof(u32) * submit->num_cmdstream_words;
	if (copy_from_user(cmdstream, user_cmdstream, size)) {
		JOB_ERROR("failed to copy cmdstream");
		return -EFAULT;
	}

	return 0;
}

static inline int
tegra_drm_resolve_reloc_bos(struct tegra_drm_job *job, void *user_data,
			    struct drm_tegra_submit_v2 *submit,
			    struct drm_file *file)
{
	struct tegra_bo **job_bos = tegra_drm_job_bos_ptr(job);
	struct drm_tegra_bo_table_entry *bo_table;
	struct drm_gem_object *gem;
	unsigned int i;

	bo_table = tegra_drm_user_data_bo_table_ptr(user_data);

	for (i = 0; i < submit->num_bos; i++) {
		gem = idr_find(&file->object_idr, bo_table[i].handle);
		if (!gem) {
			JOB_ERROR("failed to find bo handle[%u] = %u",
				  i, bo_table[i].handle);
			return -EINVAL;
		}

		job_bos[i] = to_tegra_bo(gem);
	}

	for (i = 0; i < submit->num_bos; i++) {
		gem = &job_bos[i]->gem;
		drm_gem_object_get(gem);

		if (bo_table[i].flags & DRM_TEGRA_BO_TABLE_WRITE)
			set_bit(i, job->bos_write_bitmap);
	}

	job->num_bos = submit->num_bos;

	return 0;
}

static inline int
tegra_drm_resolve_bos(struct tegra_drm_job *job, void *user_data,
		      struct drm_tegra_submit_v2 *submit,
		      struct drm_file *file)
{
	int ret;

	spin_lock(&file->table_lock);
	ret = tegra_drm_resolve_reloc_bos(job, user_data, submit, file);
	spin_unlock(&file->table_lock);

	return ret;
}

static inline int
tegra_drm_allocate_host1x_bo(struct host1x *host,
			     struct tegra_drm_job *job,
			     struct drm_tegra_submit_v2 *submit)
{
	bool from_pool = true;
	size_t bo_size;
	int err;

	/*
	 * Note that cmdstream will be appended with additional opcodes
	 * by the driver, hence reserve some extra space (8 words).
	 */
	bo_size = (submit->num_cmdstream_words + 8) * sizeof(u32);

	/*
	 * Allocate space for the CDMA push buffer data, preferring
	 * allocation from the pool.
	 */
	err = host1x_bo_alloc_data(host, &job->base.bo, bo_size, from_pool);
	if (err) {
		JOB_ERROR("failed to allocate host1x bo: %d", err);
		return err;
	}

	job->base.num_words = submit->num_cmdstream_words;

	return 0;
}

static inline int
tegra_drm_iomap_bos(struct tegra_drm *tegra, struct tegra_drm_job *job)
{
	struct tegra_bo **job_bos = tegra_drm_job_bos_ptr(job);
	long ret;

retry:
	ret = tegra_drm_job_map_gart(job, job_bos);

	if (ret == -EAGAIN) {
		ret = wait_for_completion_killable_timeout(&tegra->gart_free_up,
							   HZ);
		if (ret > 0)
			goto retry;

		if (ret == 0)
			return -ENOSPC;
	}

	return ret;
}

static inline int
tegra_drm_patch_cmdstream(struct tegra_drm *tegra,
			  struct tegra_drm_job *job, void *user_data,
			  struct drm_tegra_submit_v2 *submit)
{
	unsigned int num_incrs;
	u32 *cmdstream;
	u64 pipes;
	int err;

	cmdstream = tegra_drm_user_data_cmdstream_ptr(user_data, submit);

	/*
	 * Validate, copy and patch commands stream that is taken from
	 * userspace into the allocated push buffer.
	 */
	err = tegra_drm_copy_and_patch_cmdstream(tegra, job,
						 tegra_drm_job_bos_ptr(job),
						 submit->pipes, cmdstream,
						 &pipes, &num_incrs);
	if (err) {
		tegra_drm_debug_dump_job(job);
		return err;
	}

	job->base.num_incrs = num_incrs;
	job->pipes = pipes;

	return 0;
}

static inline int
tegra_drm_select_channel(struct tegra_drm *tegra, struct tegra_drm_job *job)
{
	struct tegra_drm_channel *best_channel = NULL;
	struct tegra_drm_channel *drm_channel;
	struct host1x_channel *channel;
	u64 pipes = job->pipes;
	int best_rating = 0;
	int rating;

	list_for_each_entry(drm_channel, &tegra->channels, list) {

		/* find channel that can handle this job */
		if ((drm_channel->acceptable_pipes & pipes) != pipes)
			continue;

		/*
		 * Select channel that fits best for this job.
		 *
		 * The 3d channel accepts both 3d / 2d (and mixed) jobs, but
		 * if job is a pure 2d-job, then it will go straightly to the
		 * 2d channel. Thus 3d channel will take only a pure 3d job
		 * or a mix of 3d / 2d.
		 */
		rating = 64 - hweight64(drm_channel->acceptable_pipes ^ pipes);

		if (rating > best_rating)
			best_channel = drm_channel;

		/*
		 * Channels are rated based on the number of used pipes that
		 * are provided by a channel, maximum rating is 64 which means
		 * that all available pipes are utilized by this job.
		 */
		if (rating == 64)
			break;
	}

	if (!best_channel) {
		JOB_ERROR("failed to select channel, pipes %llu", pipes);
		return -EINVAL;
	}

	job->drm_channel = best_channel;

	channel = best_channel->channel;

	host1x_syncpt_associate_device(job->base.syncpt, channel->dev);

	return 0;
}

static inline int
tegra_drm_lock_reservations(struct ww_acquire_ctx *acquire_ctx,
			    struct tegra_drm_job *job)
{
	struct tegra_bo **job_bos = tegra_drm_job_bos_ptr(job);
	struct dma_resv *resv;
	unsigned int num_bos = job->num_bos;
	int i, k, contended_lock = -1;
	int ret = 0;

	/*
	 * Documentation/locking/ww-mutex-design.txt recommends to avoid
	 * context setup overhead in a case of a single mutex.
	 */
	if (num_bos <= 1)
		acquire_ctx = NULL;
	else
		ww_acquire_init(acquire_ctx, &reservation_ww_class);
retry:
	if (contended_lock != -1) {
		resv = job_bos[contended_lock]->gem.resv;
		ret = ww_mutex_lock_slow_interruptible(&resv->lock,
						       acquire_ctx);
		if (ret)
			goto done;
	}

	for (i = 0; i < num_bos; i++) {
		resv = job_bos[i]->gem.resv;

		if (i == contended_lock)
			continue;

		ret = ww_mutex_lock_interruptible(&resv->lock, acquire_ctx);
		if (ret) {
			for (k = 0; k < i; k++) {
				resv = job_bos[k]->gem.resv;
				ww_mutex_unlock(&resv->lock);
			}

			if (contended_lock >= i) {
				resv = job_bos[contended_lock]->gem.resv;
				ww_mutex_unlock(&resv->lock);
			}

			if (ret == -EDEADLK) {
				contended_lock = i;
				goto retry;
			}

			if (ret == -EALREADY)
				JOB_ERROR("bo table has duplicates");

			goto done;
		}
	}
done:
	if (num_bos > 1)
		ww_acquire_done(acquire_ctx);

	return ret;
}

static inline void
tegra_drm_unlock_reservations(struct ww_acquire_ctx *acquire_ctx,
			      struct tegra_drm_job *job)
{
	struct tegra_bo **job_bos = tegra_drm_job_bos_ptr(job);
	struct dma_resv *resv;
	unsigned int i;

	for (i = 0; i < job->num_bos; i++) {
		resv = job_bos[i]->gem.resv;
		ww_mutex_unlock(&resv->lock);
	}

	if (job->num_bos > 1)
		ww_acquire_fini(acquire_ctx);
}

static inline int
tegra_drm_get_bo_fences(struct tegra_drm_job *job, void *user_data,
			struct drm_tegra_submit_v2 *submit)
{
	struct tegra_bo **job_bos = tegra_drm_job_bos_ptr(job);
	struct drm_tegra_bo_table_entry *bo_table;
	struct tegra_drm_bo_fences *fences;
	struct dma_resv *resv;
	unsigned int i, k;
	bool mem_write;
	int err;

	bo_table = tegra_drm_user_data_bo_table_ptr(user_data);
	fences = tegra_drm_job_bo_fences_ptr(job, submit);

	for (i = 0; i < job->num_bos; i++) {
		resv = job_bos[i]->gem.resv;

		if (bo_table[i].flags & DRM_TEGRA_BO_TABLE_WRITE)
			mem_write = true;
		else
			mem_write = false;

		if (bo_table[i].flags & DRM_TEGRA_BO_TABLE_EXPLICIT_FENCE) {
			if (!mem_write) {
				err = dma_resv_reserve_shared(resv, 1);
				if (err)
					goto err_put_fences;
			}

			fences[i].excl = NULL;
			fences[i].num_shared = 0;
			continue;
		}

		if (mem_write) {
			err = dma_resv_get_fences_rcu(resv,
						      &fences[i].excl,
						      &fences[i].num_shared,
						      &fences[i].shared);
			if (err)
				goto err_put_fences;
		} else {
			err = dma_resv_reserve_shared(resv, 1);
			if (err)
				goto err_put_fences;

			fences[i].excl = dma_resv_get_excl_rcu(resv);
			fences[i].num_shared = 0;
		}
	}

	job->bo_fences = fences;

	return 0;

err_put_fences:
	while (i--) {
		struct tegra_drm_bo_fences *f = &fences[i];

		dma_fence_put(f->excl);

		for (k = 0; k < f->num_shared; k++)
			dma_fence_put(f->shared[k]);
	}

	return err;
}

static inline void
tegra_drm_complete_reservations(struct ww_acquire_ctx *acquire_ctx,
				struct tegra_drm_job *job, void *user_data,
				struct dma_fence *fence)
{
	struct tegra_bo **job_bos = tegra_drm_job_bos_ptr(job);
	struct drm_tegra_bo_table_entry *bo_table;
	struct dma_resv *resv;
	unsigned int i;

	bo_table = tegra_drm_user_data_bo_table_ptr(user_data);

	for (i = 0; i < job->num_bos; i++) {
		resv = job_bos[i]->gem.resv;

		if (bo_table[i].flags & DRM_TEGRA_BO_TABLE_WRITE)
			dma_resv_add_excl_fence(resv, fence);
		else
			dma_resv_add_shared_fence(resv, fence);

		ww_mutex_unlock(&resv->lock);
	}

	if (job->num_bos > 1)
		ww_acquire_fini(acquire_ctx);

	dma_fence_put(fence);

	tegra_drm_job_put(job);
}

static inline int
tegra_drm_prepare_job(struct tegra_drm *tegra, struct tegra_drm_job *job)
{
	struct tegra_drm_client *drm_client;
	u64 pipes = job->pipes;
	int err, ret;

	list_for_each_entry(drm_client, &tegra->clients, list) {
		if (!drm_client->prepare_job)
			continue;

		if (pipes & drm_client->pipe) {
			ret = drm_client->prepare_job(drm_client, job);
			if (ret) {
				JOB_DEV_ERROR(drm_client->base.dev,
					"failed to prepare job: %d\n", ret);
				goto err_unprepare;
			}
		}
	}

	job->prepared = true;

	return 0;

err_unprepare:
	list_for_each_entry_continue_reverse(drm_client, &tegra->clients,
					     list) {
		if (!drm_client->unprepare_job)
			continue;

		err = drm_client->unprepare_job(drm_client, job);
		if (err)
			JOB_DEV_ERROR(drm_client->base.dev,
				      "failed to unprepare job: %d\n", err);
	}

	return ret;
}

static inline int
tegra_drm_schedule_job(struct tegra_drm *tegra,
		       struct tegra_drm_job *job,
		       struct drm_tegra_submit_v2 *submit,
		       struct drm_file *file,
		       struct dma_fence **pfence)
{
	struct tegra_drm_channel *drm_channel = job->drm_channel;
	struct host1x_channel *channel = drm_channel->channel;
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct drm_sched_entity *sched_entity;
	int err;

	sched_entity = &fpriv->sched_entities[channel->id];
	err = drm_sched_job_init(&job->sched_job, sched_entity, NULL);
	if (err) {
		JOB_ERROR("failed to prepare job for scheduling: %d", err);
		return err;
	}

	/* put by tegra_drm_complete_reservations() */
	tegra_drm_job_get(job);

	*pfence = dma_fence_get(&job->sched_job.s_fence->finished);

	/*
	 * Allow to re-use sync object without requiring userspace to
	 * explicitly reset its state using the corresponding IOCTL.
	 * Reset sync object now.
	 */
	if (job->out_syncobj)
		drm_syncobj_replace_fence(job->out_syncobj, *pfence);

	drm_sched_entity_push_job(&job->sched_job, sched_entity);

	return 0;
}

int tegra_drm_submit_job_v2(struct drm_device *drm,
			    struct drm_tegra_submit_v2 *submit,
			    struct drm_file *file)
{
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct tegra_drm *tegra = drm->dev_private;
	struct ww_acquire_ctx acquire_ctx;
	struct tegra_drm_job *job = NULL;
	struct dma_fence *fence;
	void *user_data = NULL;
	int err;

	err = tegra_drm_check_submit(submit);
	if (err)
		return err;

	err = tegra_drm_allocate_job(host, drm, tegra, submit, file,
				     &job, &user_data);
	if (err)
		return err;

	err = tegra_drm_allocate_host1x_bo(host, job, submit);
	if (err)
		goto err_free_job;

	err = tegra_drm_copy_user_data(job, user_data, submit);
	if (err)
		goto err_free_job;

	err = tegra_drm_resolve_bos(job, user_data, submit, file);
	if (err)
		goto err_free_job;

	err = tegra_drm_lock_reservations(&acquire_ctx, job);
	if (err)
		goto err_free_job;

	err = tegra_drm_iomap_bos(tegra, job);
	if (err)
		goto err_unlock_reservations;

	err = tegra_drm_patch_cmdstream(tegra, job, user_data, submit);
	if (err)
		goto err_unlock_reservations;

	err = tegra_drm_select_channel(tegra, job);
	if (err)
		goto err_unlock_reservations;

	err = tegra_drm_get_bo_fences(job, user_data, submit);
	if (err)
		goto err_unlock_reservations;

	err = tegra_drm_prepare_job(tegra, job);
	if (err)
		goto err_unlock_reservations;

	err = tegra_drm_schedule_job(tegra, job, submit, file, &fence);
	if (err)
		goto err_unlock_reservations;

	tegra_drm_complete_reservations(&acquire_ctx, job, user_data, fence);

	kfree(user_data);

	return 0;

err_unlock_reservations:
	tegra_drm_unlock_reservations(&acquire_ctx, job);

err_free_job:
	tegra_drm_free_job(job);

	kfree(user_data);

	return err;
}
