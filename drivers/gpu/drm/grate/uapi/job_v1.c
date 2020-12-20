/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/bitops.h>

#include "debug.h"
#include "gart.h"
#include "job.h"
#include "uapi.h"

#define JOB_ERROR(fmt, args...) \
	DRM_ERROR_RATELIMITED(fmt " (%s)\n", ##args, job->task_name)

#define JOB_DEV_ERROR(dev, fmt, args...) \
	DRM_DEV_ERROR_RATELIMITED(dev, fmt " (%s)\n", ##args, job->task_name)

struct tegra_drm_job_v1 {
	struct tegra_drm_job base;
	struct tegra_drm_context_v1 *context;
	unsigned int host1x_class;
	bool scheduled;
};

static inline struct tegra_drm_job_v1 *
to_tegra_drm_job_v1(struct tegra_drm_job *job)
{
	return container_of(job, struct tegra_drm_job_v1, base);
}

static inline struct tegra_bo **
tegra_drm_job_bos_ptr(struct tegra_drm_job *job)
{
	struct tegra_drm_job_v1 *job_v1 = to_tegra_drm_job_v1(job);

	return (struct tegra_bo **) (job_v1 + 1);
}

static inline int
tegra_drm_check_submit(struct drm_tegra_submit *submit)
{
	if (submit->num_relocs > DRM_TEGRA_BO_TABLE_MAX_ENTRIES_NUM) {
		DRM_ERROR_RATELIMITED("invalid num_relocs: %u\n",
				      submit->num_relocs);
		return -EINVAL;
	}

	return 0;
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

static void tegra_drm_free_job_v1(struct work_struct *work)
{
	struct tegra_drm_job *job = container_of(work, struct tegra_drm_job,
						 free_work);
	struct tegra_drm_job_v1 *job_v1 = to_tegra_drm_job_v1(job);
	struct tegra_drm_context_v1 *context = job_v1->context;
	atomic_t *num_active_jobs = job->num_active_jobs;

	if (job_v1->scheduled) {
		context->completed_jobs++;
		wake_up_all(&context->wq);
	}

	dma_fence_put(job->hw_fence);
	host1x_syncpt_detach_fences(job->base.syncpt);
	host1x_cleanup_job(job->host, &job->base);
	tegra_drm_context_v1_put(context);
	tegra_drm_unprepare_job(job);
	tegra_drm_put_job_bos(job);
	kfree(job_v1);

	atomic_dec(num_active_jobs);
}

static inline int
tegra_drm_allocate_job(struct host1x *host,
		       struct drm_device *drm,
		       struct tegra_drm *tegra,
		       struct drm_tegra_submit *submit,
		       struct drm_file *file,
		       struct tegra_drm_job **ret_job)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm_context_v1 *context;
	struct tegra_drm_job_v1 *job;
	struct host1x_syncpt *syncpt;
	size_t job_size;
	int err;

	syncpt = host1x_syncpt_request(host);
	if (IS_ERR(syncpt)) {
		err = PTR_ERR(syncpt);
		DRM_DEBUG("failed to request sync point: %d\n", err);
		return err;
	}

	job_size = sizeof(*job) +
		   sizeof(struct tegra_bo *) * submit->num_relocs;

	job = kzalloc(job_size, GFP_NOWAIT);
	if (!job) {
		err = -ENOMEM;
		goto err_put_syncpt;
	}

	context = tegra_uapi_context_v1_find(tegra, fpriv, submit->context);
	if (!context) {
		err = -EINVAL;
		goto err_free_job;
	}

	/*
	 * Userspace gets context ID instead of sync point threshold because
	 * sync point exposure is deprecated and unavailable. It is possible
	 * to wait only for all of the context's jobs in the scheduler's
	 * queue, this is good enough for UAPI v1.
	 */
	submit->fence = submit->context;

	tegra_drm_init_job(&job->base, tegra, NULL, NULL, syncpt,
			   fpriv->drm_context, &fpriv->num_active_jobs,
			   tegra_drm_free_job_v1);

	job->host1x_class = context->host1x_class;
	job->context = context;

	*ret_job = &job->base;

	return 0;

err_free_job:
	kfree(job);

err_put_syncpt:
	host1x_syncpt_put(syncpt);

	return err;
}

static inline int
tegra_drm_copy_and_patch_cmdbufs(struct tegra_drm_job *job,
				 struct drm_tegra_submit *submit,
				 struct drm_file *file,
				 u32 **cmdstream)
{
	struct drm_tegra_cmdbuf __user *user_cmdbufs;
	struct drm_tegra_reloc __user *user_relocs;
	struct drm_tegra_cmdstream_reloc new_reloc;
	struct drm_tegra_cmdbuf *cmdbufs;
	struct tegra_drm_job_v1 *job_v1;
	struct drm_tegra_reloc *relocs;
	struct drm_gem_object *gem;
	struct tegra_bo **job_bos;
	struct tegra_bo *bo;
	unsigned int i, k;
	size_t size;
	u64 offset;
	u32 *bufptr;
	u32 *ptr;
	int err;

	job_v1 = to_tegra_drm_job_v1(job);

	user_cmdbufs = u64_to_user_ptr(submit->cmdbufs);
	user_relocs = u64_to_user_ptr(submit->relocs);

	/* allocates space for UAPI cmdbuf descriptors */
	cmdbufs = kmalloc_array(submit->num_cmdbufs, sizeof(*cmdbufs),
				GFP_KERNEL);
	if (!cmdbufs)
		return -ENOMEM;

	/* copy cmdbuf descriptors from userspace */
	size = submit->num_cmdbufs * sizeof(*cmdbufs);
	if (copy_from_user(cmdbufs, user_cmdbufs, size)) {
		err = -EFAULT;
		goto err_free_cmdbufs;
	}

	/* job could have no relocations */
	if (submit->num_relocs) {
		/* allocates space for UAPI relocation descriptors */
		relocs = kmalloc_array(submit->num_relocs, sizeof(*relocs),
				       GFP_KERNEL);
		if (!relocs) {
			err = -ENOMEM;
			goto err_free_cmdbufs;
		}

		/* copy relocation descriptors from userspace */
		size = submit->num_relocs * sizeof(*relocs);
		if (copy_from_user(relocs, user_relocs, size)) {
			err = -EFAULT;
			goto err_free_relocs;
		}
	} else {
		relocs = NULL;
	}

	for (job->base.num_words = 1, i = 0; i < submit->num_cmdbufs; i++)
		job->base.num_words += cmdbufs[i].words;

	*cmdstream = kmalloc_array(job->base.num_words, sizeof(u32),
				   GFP_KERNEL);
	if (!*cmdstream) {
		err = -ENOMEM;
		goto err_free_relocs;
	}

	ptr = *cmdstream;

	/*
	 * New UAPI doesn't prepend job with class selection, it shall be
	 * done by the jobs cmdstream.
	 */
	*ptr++ = host1x_opcode_setclass(job_v1->host1x_class, 0, 0);

	spin_lock(&file->table_lock);

	/*
	 * New UAPI doesn't have a notion of cmdbufs, copy all cmdbufs
	 * into a single-contiguous cmdstream.
	 */
	for (i = 0; i < submit->num_cmdbufs; i++) {
		gem = idr_find(&file->object_idr, cmdbufs[i].handle);
		if (!gem) {
			JOB_ERROR("failed to find cmdbuf bo handle[%u] %u",
				  i, cmdbufs[i].handle);
			err = -EINVAL;
			goto err_unlock;
		}

		offset = (u64)cmdbufs[i].offset +
			 (u64)cmdbufs[i].words * sizeof(u32);

		if (offset & 3 || offset > gem->size - sizeof(u32)) {
			JOB_ERROR("invalid cmdbuf offset %llu", offset);
			err = -EINVAL;
			goto err_unlock;
		}

		bo = to_tegra_bo(gem);

		if (!bo->vaddr) {
			/* tegra_bo_vmap() may reschedule */
			spin_unlock(&file->table_lock);

			tegra_bo_vmap(bo);
			if (!bo->vaddr) {
				JOB_ERROR("bo not mapped");
				err = -ENOMEM;
				goto err_free_cmdstream;
			}

			spin_lock(&file->table_lock);
		}

		memcpy(ptr, bo->vaddr + cmdbufs[i].offset,
		       cmdbufs[i].words * sizeof(u32));

		ptr += cmdbufs[i].words;
	}

	if (!submit->num_relocs)
		goto unlock;

	/* new UAPI embeds relocations into cmdstream */
	job_bos = tegra_drm_job_bos_ptr(job);

	for (i = 0, k = 0, bufptr = NULL; i < submit->num_relocs; i++) {
		/*
		 * cmdbufs and relocations usually are ordered, optimize
		 * lookup for this case.
		 */
		ptr = bufptr;

		for (; ptr && k < submit->num_cmdbufs; k++) {
			if (cmdbufs[k].handle == relocs[i].cmdbuf.handle) {
				bufptr = ptr;
				goto patch_reloc;
			}

			ptr += cmdbufs[k].words;
		}

		/*
		 * Perform full lookup if the above assumption fails and on
		 * the first invocation.
		 */
		ptr = *cmdstream + 1;

		for (k = 0; k < submit->num_cmdbufs; k++) {
			if (cmdbufs[k].handle == relocs[i].cmdbuf.handle) {
				bufptr = ptr;
				break;
			}

			ptr += cmdbufs[k].words;
		}

		if (k == submit->num_cmdbufs) {
			JOB_ERROR("invalid reloc[%u] cmdbuf.handle %u",
				  i, relocs[i].cmdbuf.handle);
			err = -EINVAL;
			goto err_unlock;
		}
patch_reloc:
		offset = (u64)cmdbufs[k].offset +
			 (u64)cmdbufs[k].words * sizeof(u32);

		if (relocs[i].cmdbuf.offset >= offset ||
		    relocs[i].cmdbuf.offset & 3) {
			JOB_ERROR("invalid reloc[%u] cmdbuf.offset %u",
				  i, relocs[i].cmdbuf.offset);
			err = -EINVAL;
			goto err_unlock;
		}

		new_reloc.bo_index = i;
		new_reloc.bo_offset = relocs[i].target.offset;

		ptr += relocs[i].cmdbuf.offset / sizeof(u32);
		*ptr = new_reloc.u_data;

		gem = idr_find(&file->object_idr, relocs[i].target.handle);
		if (!gem) {
			JOB_ERROR("invalid reloc[%u] target.handle %u",
				  i, relocs[i].target.handle);
			err = -EINVAL;
			goto err_unlock;
		}

		job_bos[i] = to_tegra_bo(gem);
	}

	job->num_bos = submit->num_relocs;

	for (i = 0; i < job->num_bos; i++) {
		/* UAPI v1 doesn't have BO-write marking, mark them all */
		set_bit(i, job->bos_write_bitmap);

		gem = &job_bos[i]->gem;
		drm_gem_object_get(gem);
	}

unlock:
	spin_unlock(&file->table_lock);

	kfree(relocs);
	kfree(cmdbufs);

	return 0;

err_unlock:
	spin_unlock(&file->table_lock);
err_free_cmdstream:
	kfree(*cmdstream);
err_free_relocs:
	kfree(relocs);
err_free_cmdbufs:
	kfree(cmdbufs);

	return err;
}

static inline int
tegra_drm_allocate_host1x_bo(struct host1x *host,
			     struct tegra_drm_job *job,
			     struct drm_tegra_submit *submit)
{
	bool from_pool = true;
	size_t bo_size;
	int err;

	/*
	 * Note that cmdstream will be appended with additional opcodes
	 * by the driver, hence reserve some extra space (8 words).
	 */
	bo_size = (job->base.num_words + 8) * sizeof(u32);

	/*
	 * Allocate space for the CDMA push buffer data, preferring
	 * allocation from the pool.
	 */
	err = host1x_bo_alloc_data(host, &job->base.bo, bo_size, from_pool);
	if (err) {
		JOB_ERROR("failed to allocate host1x bo: %d", err);
		return err;
	}

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
			  struct tegra_drm_job *job,
			  u32 *words_in)
{
	unsigned int num_incrs;
	u64 pipes;
	int err;

	/*
	 * Validate, copy and patch commands stream that is taken from
	 * userspace into the allocated push buffer.
	 */
	err = tegra_drm_copy_and_patch_cmdstream(tegra, job,
						 tegra_drm_job_bos_ptr(job),
						 0xffffffffffffffff,
						 words_in, &pipes, &num_incrs);
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

	if (hweight64(pipes) > 1) {
		JOB_ERROR("uapi v1 doesn't allow to have more than one class per job, pipes %llu",
			  pipes);
		return -EINVAL;
	}

	list_for_each_entry(drm_channel, &tegra->channels, list) {

		/* find channel that can handle this job */
		if ((drm_channel->acceptable_pipes & pipes) != pipes)
			continue;

		/* select channel that fits best for this job */
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
		       struct drm_tegra_submit *submit,
		       struct drm_file *file)
{
	struct tegra_drm_job_v1 *job_v1 = to_tegra_drm_job_v1(job);
	struct tegra_drm_channel *drm_channel = job->drm_channel;
	struct tegra_drm_context_v1 *context = job_v1->context;
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

	context = tegra_drm_context_v1_get(context);
	spin_lock(&tegra->context_lock);

	submit->fence = ++context->scheduled_jobs;
	job_v1->scheduled = true;

	drm_sched_entity_push_job(&job->sched_job, sched_entity);

	spin_unlock(&tegra->context_lock);
	tegra_drm_context_v1_put(context);

	return 0;
}

int tegra_drm_submit_job_v1(struct drm_device *drm,
			    struct drm_tegra_submit *submit,
			    struct drm_file *file)
{
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_drm_job *job = NULL;
	u32 *cmdstream;
	int err;

	err = tegra_drm_check_submit(submit);
	if (err)
		return err;

	err = tegra_drm_allocate_job(host, drm, tegra, submit, file, &job);
	if (err)
		return err;

	/* this function maps older v1 job UAPI into the newer v2 */
	err = tegra_drm_copy_and_patch_cmdbufs(job, submit, file, &cmdstream);
	if (err)
		goto err_free_job;

	err = tegra_drm_allocate_host1x_bo(host, job, submit);
	if (err)
		goto err_free_cmdstream;

	err = tegra_drm_iomap_bos(tegra, job);
	if (err)
		goto err_free_cmdstream;

	err = tegra_drm_patch_cmdstream(tegra, job, cmdstream);
	if (err)
		goto err_free_cmdstream;

	err = tegra_drm_select_channel(tegra, job);
	if (err)
		goto err_free_cmdstream;

	err = tegra_drm_prepare_job(tegra, job);
	if (err)
		goto err_free_cmdstream;

	err = tegra_drm_schedule_job(tegra, job, submit, file);
	if (err)
		goto err_free_cmdstream;

	kfree(cmdstream);

	return 0;

err_free_cmdstream:
	kfree(cmdstream);

err_free_job:
	tegra_drm_free_job(job);

	return err;
}
