/*
 * Copyright (C) 2012 Avionic Design GmbH
 * Copyright (C) 2012-2016 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/bitops.h>
#include <linux/host1x.h>
#include <linux/idr.h>
#include <linux/iommu.h>
#include <linux/sync_file.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>

#include "drm.h"
#include "gem.h"

#define DRIVER_NAME "tegra"
#define DRIVER_DESC "NVIDIA Tegra graphics"
#define DRIVER_DATE "20120330"
#define DRIVER_MAJOR 0
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 0

#define CARVEOUT_SZ SZ_64M

#define TEGRA_HOST1X_MODULES_MAX_NUM		32
#define TEGRA_DRM_CONTEXTS_MAX_NUM		128

#define TEGRA_CONTEXTS_MAX_NUM						\
	(TEGRA_DRM_CONTEXTS_MAX_NUM * TEGRA_HOST1X_MODULES_MAX_NUM)

#define TEGRA_CONTEXT_DRM(ctx, base)					\
	((ctx - base) & (TEGRA_DRM_CONTEXTS_MAX_NUM - 1))

#define TEGRA_CONTEXT_MODULE(ctx, base)					\
	((ctx - base) & ~(TEGRA_DRM_CONTEXTS_MAX_NUM - 1))

#define TEGRA_CONTEXT_VALUE(drm_ctx, base, modid)			\
	(base + modid * TEGRA_DRM_CONTEXTS_MAX_NUM + drm_ctx)

struct tegra_drm_file {
	struct idr contexts;
	struct mutex lock;
	unsigned int drm_context;
};

struct tegra_bo_reservation {
	struct tegra_bo *bo;
	bool cmdbuf;
	bool write;
	bool skip;
};

static void tegra_atomic_schedule(struct tegra_drm *tegra,
				  struct drm_atomic_state *state)
{
	tegra->commit.state = state;
	schedule_work(&tegra->commit.work);
}

static void tegra_atomic_complete(struct tegra_drm *tegra,
				  struct drm_atomic_state *state)
{
	struct drm_device *drm = tegra->drm;

	/*
	 * Everything below can be run asynchronously without the need to grab
	 * any modeset locks at all under one condition: It must be guaranteed
	 * that the asynchronous work has either been cancelled (if the driver
	 * supports it, which at least requires that the framebuffers get
	 * cleaned up with drm_atomic_helper_cleanup_planes()) or completed
	 * before the new state gets committed on the software side with
	 * drm_atomic_helper_swap_state().
	 *
	 * This scheme allows new atomic state updates to be prepared and
	 * checked in parallel to the asynchronous completion of the previous
	 * update. Which is important since compositors need to figure out the
	 * composition of the next frame right after having submitted the
	 * current layout.
	 */

	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_modeset_enables(drm, state);
	drm_atomic_helper_commit_planes(drm, state,
					DRM_PLANE_COMMIT_ACTIVE_ONLY);

	drm_atomic_helper_wait_for_vblanks(drm, state);

	drm_atomic_helper_cleanup_planes(drm, state);
	drm_atomic_state_put(state);
}

static void tegra_atomic_work(struct work_struct *work)
{
	struct tegra_drm *tegra = container_of(work, struct tegra_drm,
					       commit.work);

	tegra_atomic_complete(tegra, tegra->commit.state);
}

static int tegra_atomic_commit(struct drm_device *drm,
			       struct drm_atomic_state *state, bool nonblock)
{
	struct tegra_drm *tegra = drm->dev_private;
	int err;

	err = drm_atomic_helper_prepare_planes(drm, state);
	if (err)
		return err;

	/* serialize outstanding nonblocking commits */
	mutex_lock(&tegra->commit.lock);
	flush_work(&tegra->commit.work);

	/*
	 * This is the point of no return - everything below never fails except
	 * when the hw goes bonghits. Which means we can commit the new state on
	 * the software side now.
	 */

	err = drm_atomic_helper_swap_state(state, true);
	if (err) {
		mutex_unlock(&tegra->commit.lock);
		drm_atomic_helper_cleanup_planes(drm, state);
		return err;
	}

	drm_atomic_state_get(state);
	if (nonblock)
		tegra_atomic_schedule(tegra, state);
	else
		tegra_atomic_complete(tegra, state);

	mutex_unlock(&tegra->commit.lock);
	return 0;
}

static const struct drm_mode_config_funcs tegra_drm_mode_funcs = {
	.fb_create = tegra_fb_create,
#ifdef CONFIG_DRM_FBDEV_EMULATION
	.output_poll_changed = tegra_fb_output_poll_changed,
#endif
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = tegra_atomic_commit,
};

static int tegra_drm_iova_init(struct tegra_drm *tegra,
			       u64 carveout_start, u64 carveout_end)
{
	unsigned long order = __ffs(tegra->domain->pgsize_bitmap);

	if (of_machine_is_compatible("nvidia,tegra20"))
		return 0;

	tegra->carveout = kzalloc(sizeof(*tegra->carveout), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	init_iova_domain(&tegra->carveout->domain, 1UL << order,
			 carveout_start >> order);

	tegra->carveout->shift = iova_shift(&tegra->carveout->domain);
	tegra->carveout->limit = carveout_end >> tegra->carveout->shift;

	DRM_DEBUG("  Carveout: %#llx-%#llx\n", carveout_start, carveout_end);

	return 0;
}

static int tegra_drm_iommu_init(struct tegra_drm *tegra)
{
	struct iommu_domain_geometry *geometry;
	u64 gem_start, gem_end;
	int err;

	if (!iommu_present(&platform_bus_type))
		return 0;

	tegra->domain = iommu_domain_alloc(&platform_bus_type);
	if (!tegra->domain)
		return -ENOMEM;

	geometry = &tegra->domain->geometry;
	gem_start = geometry->aperture_start;
	gem_end = geometry->aperture_end;

	/* the whole GART aperture is smaller than CARVEOUT_SZ on Tegra20 */
	if (!of_machine_is_compatible("nvidia,tegra20"))
		gem_end -= CARVEOUT_SZ;

	/* do not waste precious GART aperture on Tegra20 */
	if (of_machine_is_compatible("nvidia,tegra20"))
		tegra->dynamic_iommu_mapping = true;

	drm_mm_init(&tegra->mm, gem_start, gem_end - gem_start + 1);
	mutex_init(&tegra->mm_lock);

	DRM_DEBUG("IOMMU apertures:\n");
	DRM_DEBUG("  GEM: %#llx-%#llx\n", gem_start, gem_end);

	err = tegra_drm_iova_init(tegra, gem_end + 1, geometry->aperture_end);
	if (err) {
		iommu_domain_free(tegra->domain);
		return err;
	}

	INIT_LIST_HEAD(&tegra->mm_eviction_list);

	return 0;
}

static int tegra_drm_load(struct drm_device *drm, unsigned long flags)
{
	struct host1x_device *device = to_host1x_device(drm->dev);
	struct tegra_drm *tegra;
	int err;

	tegra = kzalloc(sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	err = tegra_drm_iommu_init(tegra);
	if (err)
		goto free;

	mutex_init(&tegra->lock);
	INIT_LIST_HEAD(&tegra->clients);

	mutex_init(&tegra->commit.lock);
	INIT_WORK(&tegra->commit.work, tegra_atomic_work);

	drm->dev_private = tegra;
	tegra->drm = drm;

	drm_mode_config_init(drm);

	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;

	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;

	drm->mode_config.allow_fb_modifiers = true;

	drm->mode_config.funcs = &tegra_drm_mode_funcs;

	err = tegra_drm_fb_prepare(drm);
	if (err < 0)
		goto config;

	drm_kms_helper_poll_init(drm);

	err = host1x_device_init(device);
	if (err < 0)
		goto fbdev;

	/*
	 * We don't use the drm_irq_install() helpers provided by the DRM
	 * core, so we need to set this manually in order to allow the
	 * DRM_IOCTL_WAIT_VBLANK to operate correctly.
	 */
	drm->irq_enabled = true;

	/* syncpoints are used for full 32-bit hardware VBLANK counters */
	drm->max_vblank_count = 0xffffffff;

	err = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (err < 0)
		goto device;

	drm_mode_config_reset(drm);

	err = tegra_drm_fb_init(drm);
	if (err < 0)
		goto device;

	/*
	 * We need to differentiate BO's coming from a different DRM context
	 * (shared BO's) and we also need to differentiate Host1x module
	 * that uses BO, because BO's within the same module do not need
	 * to be awaited since they will be 'naturally' serialized. We
	 * do not need to wait for BO's within the same DRM context, because
	 * we need flexibility of manual BO's synchronization using waitchecks
	 * to reduce channels blocking and jobs submission overhead.
	 *
	 * The Host1x's module ID gives the context base value, so that:
	 *
	 * ctx = module_id * TEGRA_DRM_CONTEXTS_MAX_NUM + drm_ctx
	 *
	 */
	tegra->drm_contexts = kcalloc(BITS_TO_LONGS(TEGRA_DRM_CONTEXTS_MAX_NUM),
				      sizeof(unsigned long), GFP_KERNEL);

	tegra->fence_context_base =
			dma_fence_context_alloc(TEGRA_CONTEXTS_MAX_NUM);

	return 0;

device:
	host1x_device_exit(device);
fbdev:
	drm_kms_helper_poll_fini(drm);
	tegra_drm_fb_free(drm);
config:
	drm_mode_config_cleanup(drm);

	if (tegra->domain) {
		iommu_domain_free(tegra->domain);
		drm_mm_takedown(&tegra->mm);
		mutex_destroy(&tegra->mm_lock);
	}

	if (tegra->carveout) {
		put_iova_domain(&tegra->carveout->domain);
		kfree(tegra->carveout);
	}

free:
	kfree(tegra);
	return err;
}

static void tegra_drm_unload(struct drm_device *drm)
{
	struct host1x_device *device = to_host1x_device(drm->dev);
	struct tegra_drm *tegra = drm->dev_private;
	int err;

	drm_kms_helper_poll_fini(drm);
	tegra_drm_fb_exit(drm);
	drm_mode_config_cleanup(drm);

	err = host1x_device_exit(device);
	if (err < 0)
		return;

	if (tegra->domain) {
		iommu_domain_free(tegra->domain);
		drm_mm_takedown(&tegra->mm);
		mutex_destroy(&tegra->mm_lock);
	}

	if (tegra->carveout) {
		put_iova_domain(&tegra->carveout->domain);
		kfree(tegra->carveout);
	}

	kfree(tegra);
}

static int tegra_drm_open(struct drm_device *drm, struct drm_file *filp)
{
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_drm_file *fpriv;

	fpriv = kzalloc(sizeof(*fpriv), GFP_KERNEL);
	if (!fpriv)
		return -ENOMEM;

	mutex_lock(&tegra->lock);
	fpriv->drm_context = find_first_zero_bit(tegra->drm_contexts,
						 TEGRA_DRM_CONTEXTS_MAX_NUM);
	if (fpriv->drm_context < TEGRA_DRM_CONTEXTS_MAX_NUM)
		set_bit(fpriv->drm_context, tegra->drm_contexts);
	mutex_unlock(&tegra->lock);

	if (fpriv->drm_context >= TEGRA_DRM_CONTEXTS_MAX_NUM) {
		kfree(fpriv);
		return -EBUSY;
	}

	idr_init(&fpriv->contexts);
	mutex_init(&fpriv->lock);
	filp->driver_priv = fpriv;

	return 0;
}

static void tegra_drm_context_free(struct tegra_drm_context *context)
{
	context->client->ops->close_channel(context);
	kfree(context);
}

static void tegra_drm_lastclose(struct drm_device *drm)
{
#ifdef CONFIG_DRM_FBDEV_EMULATION
	struct tegra_drm *tegra = drm->dev_private;

	tegra_fbdev_restore_mode(tegra->fbdev);
#endif
}

static struct host1x_bo *
host1x_bo_lookup(struct drm_file *file, u32 handle)
{
	struct drm_gem_object *gem;
	struct tegra_bo *bo;

	gem = drm_gem_object_lookup(file, handle);
	if (!gem)
		return NULL;

	bo = to_tegra_bo(gem);
	return &bo->base;
}

static int host1x_reloc_copy_from_user(struct host1x_reloc *dest, u32 *flags,
				       struct drm_tegra_reloc __user *src,
				       struct drm_device *drm,
				       struct drm_file *file)
{
	u32 cmdbuf, target;
	int err;

	err = get_user(cmdbuf, &src->cmdbuf.handle);
	if (err < 0)
		return err;

	err = get_user(dest->cmdbuf.offset, &src->cmdbuf.offset);
	if (err < 0)
		return err;

	err = get_user(target, &src->target.handle);
	if (err < 0)
		return err;

	err = get_user(dest->target.offset, &src->target.offset);
	if (err < 0)
		return err;

	err = get_user(dest->shift, &src->shift);
	if (err < 0)
		return err;

	err = get_user(*flags, &src->flags);
	if (err < 0)
		return err;

	dest->cmdbuf.bo = host1x_bo_lookup(file, cmdbuf);
	if (!dest->cmdbuf.bo)
		return -ENOENT;

	dest->target.bo = host1x_bo_lookup(file, target);
	if (!dest->target.bo)
		return -ENOENT;

	return 0;
}

static int host1x_waitchk_copy_from_user(struct host1x_waitchk *dest,
					 struct drm_tegra_waitchk __user *src,
					 struct drm_file *file)
{
	u32 cmdbuf;
	int err;

	err = get_user(cmdbuf, &src->handle);
	if (err < 0)
		return err;

	err = get_user(dest->offset, &src->offset);
	if (err < 0)
		return err;

	err = get_user(dest->syncpt_id, &src->syncpt);
	if (err < 0)
		return err;

	err = get_user(dest->thresh, &src->thresh);
	if (err < 0)
		return err;

	dest->bo = host1x_bo_lookup(file, cmdbuf);
	if (!dest->bo)
		return -ENOENT;

	return 0;
}

static int tegra_append_bo_reservations(struct tegra_bo_reservation *resv,
					struct tegra_bo *bo, unsigned int index,
					bool write, bool cmdbuf, bool skip)
{
	if (bo->resv_pending) {
		if (resv[bo->resv_index].cmdbuf != cmdbuf)
			return -EINVAL;

		resv[index].bo = bo;
		resv[index].skip = true;
		resv[index].cmdbuf = cmdbuf;
		resv[bo->resv_index].write |= write;
	} else {
		bo->resv_index = index;
		bo->resv_pending = true;

		resv[index].bo = bo;
		resv[index].skip = skip;
		resv[index].write = write;
		resv[index].cmdbuf = cmdbuf;
	}

	return 0;
}

static int tegra_lock_bo_reservations(struct ww_acquire_ctx *acquire_ctx,
				      struct tegra_bo_reservation *bos,
				      unsigned int num_bos)
{
	struct reservation_object *resv;
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
		resv = bos[contended_lock].bo->resv;
		ret = ww_mutex_lock_slow_interruptible(&resv->lock,
						       acquire_ctx);
		if (ret)
			goto done;
	}

	for (i = 0; i < num_bos; i++) {
		/*
		 * Duplicated reservations cause a crash on the
		 * ww_mutex locking, so let's avoid these duplicates.
		 */
		if (bos[i].skip)
			continue;

		if (i == contended_lock)
			continue;

		ret = ww_mutex_lock_interruptible(&bos[i].bo->resv->lock,
						  acquire_ctx);
		if (ret) {
			for (k = 0; k < i; k++) {
				if (!bos[i].skip) {
					resv = bos[k].bo->resv;
					ww_mutex_unlock(&resv->lock);
				}
			}

			if (contended_lock >= i) {
				resv = bos[contended_lock].bo->resv;
				ww_mutex_unlock(&resv->lock);
			}

			if (ret == -EDEADLK) {
				contended_lock = i;
				goto retry;
			}

			goto done;
		}
	}
done:
	if (num_bos > 1)
		ww_acquire_done(acquire_ctx);

	return ret;
}

static void tegra_unlock_bo_reservations(struct ww_acquire_ctx *acquire_ctx,
					 struct tegra_bo_reservation *bos,
					 unsigned int num_bos)
{
	unsigned int i;

	for (i = 0; i < num_bos; i++) {
		if (bos[i].skip)
			continue;

		ww_mutex_unlock(&bos[i].bo->resv->lock);
	}

	if (num_bos > 1)
		ww_acquire_fini(acquire_ctx);
}

static int tegra_prealloc_reservations_space(struct tegra_bo_reservation *bos,
					     unsigned int num_bos)
{
	unsigned int i;
	int err;

	for (i = 0; i < num_bos; i++) {
		if (bos[i].skip)
			continue;

		/* write is exclusive, it doesn't need to be reserved */
		if (bos[i].write)
			continue;

		/* read is shared */
		err = reservation_object_reserve_shared(bos[i].bo->resv);
		if (err)
			return err;
	}

	return 0;
}

static bool tegra_fence_context_match(u64 drm_context, u64 fence_context_base,
				      u64 fence_context)
{
	/* Check whether fence was produced by Tegra's DRM */
	if (fence_context - fence_context_base >= TEGRA_CONTEXTS_MAX_NUM)
		return false;

	/* Check whether fence belongs to the same Tegra's DRM context */
	if (TEGRA_CONTEXT_DRM(drm_context, fence_context_base) ==
	    TEGRA_CONTEXT_DRM(fence_context, fence_context_base))
		return true;

	/*
	 * We don't need to wait for fence if it is in-use by the same
	 * Host1x module because BO's are naturally serialized.
	 */
	if (TEGRA_CONTEXT_MODULE(drm_context, fence_context_base) ==
	    TEGRA_CONTEXT_MODULE(fence_context, fence_context_base))
		return true;

	return false;
}

static int tegra_await_bo(struct host1x_job *job, struct tegra_bo *bo,
			  struct host1x_client *client, bool write,
			  u64 drm_context, u64 fence_context_base)
{
	struct reservation_object *resv = bo->resv;
	struct reservation_object_list *fobj;
	struct dma_fence *f;
	unsigned int i;
	int err;

	fobj = reservation_object_get_list(resv);

	/* exclusive (write) fence supersedes all shared (read) fences */
	if (!fobj || !fobj->shared_count) {
		f = reservation_object_get_excl(resv);

		/* this BO doesn't have any fences at all */
		if (!f)
			return 0;

		if (!tegra_fence_context_match(drm_context,
					       fence_context_base,
					       f->context))
		{
			if (host1x_fence_is_waitable(f))
				err = host1x_job_add_fence(job, f);
			else
				err = dma_fence_wait(f, true);

			if (err)
				return err;
		}
	}

	if (!fobj)
		return 0;

	/*
	 * On read:  BO waits for all previous writes completion.
	 * On write: BO waits for all previous writes and reads completion.
	 */
	if (!write)
		return 0;

	for (i = 0; i < fobj->shared_count; i++) {
		f = rcu_dereference_protected(fobj->shared[i],
					      reservation_object_held(resv));

		if (!tegra_fence_context_match(drm_context,
					       fence_context_base,
					       f->context))
		{
			if (host1x_fence_is_waitable(f))
				err = host1x_job_add_fence(job, f);
			else
				err = dma_fence_wait(f, true);

			if (err)
				return err;
		}
	}

	return 0;
}

static int tegra_await_bo_fences(struct host1x_job *job,
				 struct tegra_bo_reservation *bos,
				 unsigned int num_bos,
				 struct host1x_client *client,
				 u64 drm_context, u64 fence_context_base)
{
	unsigned int i;
	int err;

	for (i = 0; i < num_bos; i++) {
		if (bos[i].skip)
			continue;

		err = tegra_await_bo(job, bos[i].bo, client, bos[i].write,
				     drm_context, fence_context_base);
		if (err)
			return err;
	}

	return 0;
}

static void tegra_attach_fence(struct tegra_bo_reservation *bos,
			       unsigned int num_bos,
			       struct dma_fence *fence)
{
	unsigned int i;

	for (i = 0; i < num_bos; i++) {
		if (bos[i].skip)
			continue;

		/*
		 * Fence could signal during the 'attaching', in that case
		 * we won't attach the expired fence to the rest of the BO's,
		 * optimizing things a tad.
		 */
		if (fence && dma_fence_is_signaled(fence))
			fence = NULL;

		if (bos[i].write)
			reservation_object_add_excl_fence(bos[i].bo->resv,
							  fence);
		else if (fence)
			reservation_object_add_shared_fence(bos[i].bo->resv,
							    fence);
	}
}

int tegra_drm_submit(struct tegra_drm_context *context,
		     struct drm_tegra_submit *args, struct drm_device *drm,
		     struct drm_file *file)
{
	unsigned int num_cmdbufs = args->num_cmdbufs;
	unsigned int num_relocs = args->num_relocs;
	unsigned int num_waitchks = args->num_waitchks;
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm *tegra = drm->dev_private;
	struct drm_tegra_cmdbuf __user *user_cmdbufs;
	struct drm_tegra_reloc __user *user_relocs;
	struct drm_tegra_waitchk __user *user_waitchks;
	struct drm_tegra_syncpt __user *user_syncpt;
	struct drm_tegra_syncpt syncpt;
	struct host1x_syncpt *sp;
	struct host1x_job *job;
	struct host1x *host1x;
	struct tegra_bo_reservation *reservations;
	struct ww_acquire_ctx acquire_ctx;
	struct dma_fence *out_fence;
	struct dma_fence *in_fence;
	unsigned int num_bos;
	u64 context_value;
	int err;

	user_cmdbufs = u64_to_user_ptr(args->cmdbufs);
	user_relocs = u64_to_user_ptr(args->relocs);
	user_waitchks = u64_to_user_ptr(args->waitchks);
	user_syncpt = u64_to_user_ptr(args->syncpts);

	/* We don't yet support other than one syncpt_incr struct per submit */
	if (args->num_syncpts != 1)
		return -EINVAL;

	/* We don't yet support waitchks */
	if (args->num_waitchks != 0)
		return -EINVAL;

	job = host1x_job_alloc(context->channel, args->num_cmdbufs,
			       args->num_relocs, args->num_waitchks);
	if (!job)
		return -ENOMEM;

	job->num_relocs = args->num_relocs;
	job->num_waitchk = args->num_waitchks;
	job->client = &context->client->base;
	job->class = context->client->base.class;
	job->serialize = true;

	/* Get and await the in-fence if needed */
	if (args->flags & DRM_TEGRA_SUBMIT_WAIT_FENCE_FD) {
		in_fence = sync_file_get_fence(args->fence);
		if (!in_fence) {
			err = -ENOENT;
			goto put;
		}

		if (host1x_fence_is_waitable(in_fence))
			err = host1x_job_add_fence(job, in_fence);
		else
			err = dma_fence_wait(in_fence, true);

		/* balance in-fence reference counter */
		dma_fence_put(in_fence);

		if (err)
			goto put;
	}

	num_bos = num_cmdbufs + num_relocs * 2 + num_waitchks;

	reservations = kmalloc_array(num_bos, sizeof(*reservations),
				     GFP_KERNEL);
	if (!reservations) {
		err = -ENOMEM;
		goto put;
	}

	/* reuse as an iterator later */
	num_bos = 0;

	while (num_cmdbufs) {
		struct drm_tegra_cmdbuf cmdbuf;
		struct host1x_bo *bo;
		struct tegra_bo *obj;
		bool skip;

		if (copy_from_user(&cmdbuf, user_cmdbufs, sizeof(cmdbuf))) {
			err = -EFAULT;
			goto fail;
		}

		bo = host1x_bo_lookup(file, cmdbuf.handle);
		if (!bo) {
			err = -ENOENT;
			goto fail;
		}

		obj = host1x_to_tegra_bo(bo);

		host1x_job_add_gather(job, bo, cmdbuf.words, cmdbuf.offset);
		num_cmdbufs--;
		user_cmdbufs++;

		/*
		 * We don't care about cmdbufs reservation if firewall
		 * is enabled because their BOs will be cloned.
		 */
		skip = IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL);

		err = tegra_append_bo_reservations(reservations, obj, num_bos++,
						   false, true, skip);
		if (err)
			goto fail;
	}

	/* copy and resolve relocations from submit */
	while (num_relocs--) {
		struct host1x_reloc *reloc;
		struct tegra_bo *obj;
		u32 reloc_flags;

		err = host1x_reloc_copy_from_user(&job->relocarray[num_relocs],
						  &reloc_flags,
						  &user_relocs[num_relocs],
						  drm, file);
		if (err < 0)
			goto fail;

		reloc = &job->relocarray[num_relocs];
		obj = host1x_to_tegra_bo(reloc->cmdbuf.bo);

		err = tegra_append_bo_reservations(reservations, obj, num_bos++,
					!(reloc_flags & DRM_TEGRA_RELOC_READ_MADV),
					true, true);
		if (err)
			goto fail;

		obj = host1x_to_tegra_bo(reloc->target.bo);

		err = tegra_append_bo_reservations(reservations, obj, num_bos++,
					!(reloc_flags & DRM_TEGRA_RELOC_READ_MADV),
					false, false);
		if (err)
			goto fail;
	}

	/* copy and resolve waitchks from submit */
	while (num_waitchks--) {
		struct host1x_waitchk *wait = &job->waitchk[num_waitchks];
		struct tegra_bo *obj;

		err = host1x_waitchk_copy_from_user(wait,
						    &user_waitchks[num_waitchks],
						    file);
		if (err < 0)
			goto fail;

		obj = host1x_to_tegra_bo(wait->bo);

		err = tegra_append_bo_reservations(reservations, obj, num_bos++,
						   false, true, true);
		if (err)
			goto fail;
	}

	if (copy_from_user(&syncpt, user_syncpt, sizeof(syncpt))) {
		err = -EFAULT;
		goto fail;
	}

	job->is_addr_reg = context->client->ops->is_addr_reg;
	job->is_valid_class = context->client->ops->is_valid_class;
	job->syncpt_incrs = syncpt.incrs;
	job->syncpt_id = syncpt.id;
	job->timeout = 10000;

	if (args->timeout && args->timeout < 10000)
		job->timeout = args->timeout;

	/* acquire every BO reservation lock */
	err = tegra_lock_bo_reservations(&acquire_ctx, reservations, num_bos);
	if (err)
		goto fail;

	/* reserve space for the fences */
	err = tegra_prealloc_reservations_space(reservations, num_bos);
	if (err)
		goto fail_unlock;

	/* derive DRM's client context value */
	context_value = TEGRA_CONTEXT_VALUE(fpriv->drm_context,
					    tegra->fence_context_base,
					    context->client->base.module);

	/* await every fence of every BO */
	err = tegra_await_bo_fences(job, reservations, num_bos, job->client,
				    context_value, tegra->fence_context_base);
	if (err)
		goto fail_unlock;

	err = host1x_job_pin(job, context->client->base.dev);
	if (err)
		goto fail_unlock;

	err = host1x_job_submit(job);
	if (err) {
		host1x_job_unpin(job);
		goto fail_unlock;
	}

	/* create dma_fence for this job */
	host1x = dev_get_drvdata(drm->dev->parent);
	sp = host1x_syncpt_get(host1x, syncpt.id);
	out_fence = host1x_fence_create(sp, job->syncpt_end, context_value,
					tegra->fence_seqno++);

	/* attach fence to BO's for the further submission reservations */
	tegra_attach_fence(reservations, num_bos, out_fence);

	/* add out-fence into Sync File if needed */
	if (args->flags & DRM_TEGRA_SUBMIT_CREATE_FENCE_FD) {
		struct sync_file *sync_file;
		int fence_fd;

		if (!out_fence) {
			err = -ENOMEM;
			goto fail_unlock;
		}

		fence_fd = get_unused_fd_flags(O_CLOEXEC);
		if (fence_fd < 0) {
			err = fence_fd;
			goto fail_put_fence;
		}

		sync_file = sync_file_create(out_fence);
		if (!sync_file) {
			put_unused_fd(fence_fd);
			err = -ENOMEM;
			goto fail_put_fence;
		}

		/*
		 * Bump fences reference counter in order to keep it
		 * alive till sync file get closed.
		 */
		dma_fence_get(out_fence);

		fd_install(fence_fd, sync_file->file);
		args->fence = fence_fd;
	} else
		args->fence = job->syncpt_end;

fail_put_fence:
	dma_fence_put(out_fence);

fail_unlock:
	tegra_unlock_bo_reservations(&acquire_ctx, reservations, num_bos);

fail:
	while (num_bos--) {
		drm_gem_object_put_unlocked(&reservations[num_bos].bo->gem);
		reservations[num_bos].bo->resv_pending = false;
	}

	kfree(reservations);

put:
	host1x_job_put(job);
	return err;
}


#ifdef CONFIG_DRM_TEGRA_STAGING
static int tegra_gem_create(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct drm_tegra_gem_create *args = data;
	struct tegra_bo *bo;

	bo = tegra_bo_create_with_handle(file, drm, args->size, args->flags,
					 &args->handle);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	return 0;
}

static int tegra_gem_mmap(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_tegra_gem_mmap *args = data;
	struct drm_gem_object *gem;
	struct tegra_bo *bo;

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -EINVAL;

	bo = to_tegra_bo(gem);

	args->offset = drm_vma_node_offset_addr(&bo->gem.vma_node);

	drm_gem_object_put_unlocked(gem);

	return 0;
}

static int tegra_syncpt_read(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct drm_tegra_syncpt_read *args = data;
	struct host1x_syncpt *sp;

	sp = host1x_syncpt_get(host, args->id);
	if (!sp)
		return -EINVAL;

	args->value = host1x_syncpt_read_min(sp);
	return 0;
}

static int tegra_syncpt_incr(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct host1x *host1x = dev_get_drvdata(drm->dev->parent);
	struct drm_tegra_syncpt_incr *args = data;
	struct host1x_syncpt *sp;

	sp = host1x_syncpt_get(host1x, args->id);
	if (!sp)
		return -EINVAL;

	return host1x_syncpt_incr(sp);
}

static int tegra_syncpt_wait(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct host1x *host1x = dev_get_drvdata(drm->dev->parent);
	struct drm_tegra_syncpt_wait *args = data;
	struct host1x_syncpt *sp;

	sp = host1x_syncpt_get(host1x, args->id);
	if (!sp)
		return -EINVAL;

	return host1x_syncpt_wait(sp, args->thresh, args->timeout,
				  &args->value);
}

static int tegra_client_open(struct tegra_drm_file *fpriv,
			     struct tegra_drm_client *client,
			     struct tegra_drm_context *context)
{
	int err;

	err = client->ops->open_channel(client, context);
	if (err < 0)
		return err;

	err = idr_alloc(&fpriv->contexts, context, 1, 0, GFP_KERNEL);
	if (err < 0) {
		client->ops->close_channel(context);
		return err;
	}

	context->client = client;
	context->id = err;

	return 0;
}

static int tegra_open_channel(struct drm_device *drm, void *data,
			      struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm *tegra = drm->dev_private;
	struct drm_tegra_open_channel *args = data;
	struct tegra_drm_context *context;
	struct tegra_drm_client *client;
	int err = -ENODEV;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	mutex_lock(&fpriv->lock);

	list_for_each_entry(client, &tegra->clients, list)
		if (client->base.class == args->client) {
			err = tegra_client_open(fpriv, client, context);
			if (err < 0)
				break;

			args->context = context->id;
			break;
		}

	if (err < 0)
		kfree(context);

	mutex_unlock(&fpriv->lock);
	return err;
}

static int tegra_close_channel(struct drm_device *drm, void *data,
			       struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct drm_tegra_close_channel *args = data;
	struct tegra_drm_context *context;
	int err = 0;

	mutex_lock(&fpriv->lock);

	context = idr_find(&fpriv->contexts, args->context);
	if (!context) {
		err = -EINVAL;
		goto unlock;
	}

	idr_remove(&fpriv->contexts, context->id);
	tegra_drm_context_free(context);

unlock:
	mutex_unlock(&fpriv->lock);
	return err;
}

static int tegra_get_syncpt(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct drm_tegra_get_syncpt *args = data;
	struct tegra_drm_context *context;
	struct host1x_syncpt *syncpt;
	int err = 0;

	mutex_lock(&fpriv->lock);

	context = idr_find(&fpriv->contexts, args->context);
	if (!context) {
		err = -ENODEV;
		goto unlock;
	}

	if (args->index >= context->client->base.num_syncpts) {
		err = -EINVAL;
		goto unlock;
	}

	syncpt = context->client->base.syncpts[args->index];
	args->id = host1x_syncpt_id(syncpt);

unlock:
	mutex_unlock(&fpriv->lock);
	return err;
}

static int tegra_submit(struct drm_device *drm, void *data,
			struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct drm_tegra_submit *args = data;
	struct tegra_drm_context *context;
	int err;

	mutex_lock(&fpriv->lock);

	context = idr_find(&fpriv->contexts, args->context);
	if (!context) {
		err = -ENODEV;
		goto unlock;
	}

	err = context->client->ops->submit(context, args, drm, file);

unlock:
	mutex_unlock(&fpriv->lock);
	return err;
}

static int tegra_get_syncpt_base(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct drm_tegra_get_syncpt_base *args = data;
	struct tegra_drm_context *context;
	struct host1x_syncpt_base *base;
	struct host1x_syncpt *syncpt;
	int err = 0;

	mutex_lock(&fpriv->lock);

	context = idr_find(&fpriv->contexts, args->context);
	if (!context) {
		err = -ENODEV;
		goto unlock;
	}

	if (args->syncpt >= context->client->base.num_syncpts) {
		err = -EINVAL;
		goto unlock;
	}

	syncpt = context->client->base.syncpts[args->syncpt];

	base = host1x_syncpt_get_base(syncpt);
	if (!base) {
		err = -ENXIO;
		goto unlock;
	}

	args->id = host1x_syncpt_base_id(base);

unlock:
	mutex_unlock(&fpriv->lock);
	return err;
}

static int tegra_gem_set_tiling(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_tegra_gem_set_tiling *args = data;
	enum tegra_bo_tiling_mode mode;
	struct drm_gem_object *gem;
	unsigned long value = 0;
	struct tegra_bo *bo;

	switch (args->mode) {
	case DRM_TEGRA_GEM_TILING_MODE_PITCH:
		mode = TEGRA_BO_TILING_MODE_PITCH;

		if (args->value != 0)
			return -EINVAL;

		break;

	case DRM_TEGRA_GEM_TILING_MODE_TILED:
		mode = TEGRA_BO_TILING_MODE_TILED;

		if (args->value != 0)
			return -EINVAL;

		break;

	case DRM_TEGRA_GEM_TILING_MODE_BLOCK:
		mode = TEGRA_BO_TILING_MODE_BLOCK;

		if (args->value > 5)
			return -EINVAL;

		value = args->value;
		break;

	default:
		return -EINVAL;
	}

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;

	bo = to_tegra_bo(gem);

	bo->tiling.mode = mode;
	bo->tiling.value = value;

	drm_gem_object_put_unlocked(gem);

	return 0;
}

static int tegra_gem_get_tiling(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_tegra_gem_get_tiling *args = data;
	struct drm_gem_object *gem;
	struct tegra_bo *bo;
	int err = 0;

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;

	bo = to_tegra_bo(gem);

	switch (bo->tiling.mode) {
	case TEGRA_BO_TILING_MODE_PITCH:
		args->mode = DRM_TEGRA_GEM_TILING_MODE_PITCH;
		args->value = 0;
		break;

	case TEGRA_BO_TILING_MODE_TILED:
		args->mode = DRM_TEGRA_GEM_TILING_MODE_TILED;
		args->value = 0;
		break;

	case TEGRA_BO_TILING_MODE_BLOCK:
		args->mode = DRM_TEGRA_GEM_TILING_MODE_BLOCK;
		args->value = bo->tiling.value;
		break;

	default:
		err = -EINVAL;
		break;
	}

	drm_gem_object_put_unlocked(gem);

	return err;
}

static int tegra_gem_set_flags(struct drm_device *drm, void *data,
			       struct drm_file *file)
{
	struct drm_tegra_gem_set_flags *args = data;
	struct drm_gem_object *gem;
	struct tegra_bo *bo;

	if (args->flags & ~DRM_TEGRA_GEM_FLAGS)
		return -EINVAL;

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;

	bo = to_tegra_bo(gem);
	bo->flags = 0;

	if (args->flags & DRM_TEGRA_GEM_BOTTOM_UP)
		bo->flags |= TEGRA_BO_BOTTOM_UP;

	drm_gem_object_put_unlocked(gem);

	return 0;
}

static int tegra_gem_get_flags(struct drm_device *drm, void *data,
			       struct drm_file *file)
{
	struct drm_tegra_gem_get_flags *args = data;
	struct drm_gem_object *gem;
	struct tegra_bo *bo;

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;

	bo = to_tegra_bo(gem);
	args->flags = 0;

	if (bo->flags & TEGRA_BO_BOTTOM_UP)
		args->flags |= DRM_TEGRA_GEM_BOTTOM_UP;

	drm_gem_object_put_unlocked(gem);

	return 0;
}

static int tegra_gem_cpu_prep(struct drm_device *drm, void *data,
			      struct drm_file *file)
{
	struct drm_tegra_gem_cpu_prep *args = data;
	struct drm_gem_object *gem;
	struct tegra_bo *bo;
	unsigned long timeout;
	bool write;
	int ret;

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;

	bo = to_tegra_bo(gem);
	write = args->flags & DRM_TEGRA_CPU_PREP_WRITE;
	timeout = msecs_to_jiffies(args->timeout);

	ret = reservation_object_wait_timeout_rcu(bo->resv, write,
						  true, timeout);

	drm_gem_object_put_unlocked(gem);

	if (ret == 0)
		return timeout == 0 ? -EBUSY : -ETIMEDOUT;

	if (ret < 0)
		return ret;

	return 0;
}
#endif

static const struct drm_ioctl_desc tegra_drm_ioctls[] = {
#ifdef CONFIG_DRM_TEGRA_STAGING
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_CREATE, tegra_gem_create,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_MMAP, tegra_gem_mmap,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_SYNCPT_READ, tegra_syncpt_read,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_SYNCPT_INCR, tegra_syncpt_incr,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW | DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(TEGRA_SYNCPT_WAIT, tegra_syncpt_wait,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_OPEN_CHANNEL, tegra_open_channel,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_CLOSE_CHANNEL, tegra_close_channel,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_GET_SYNCPT, tegra_get_syncpt,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_SUBMIT, tegra_submit,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_GET_SYNCPT_BASE, tegra_get_syncpt_base,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_SET_TILING, tegra_gem_set_tiling,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_GET_TILING, tegra_gem_get_tiling,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_SET_FLAGS, tegra_gem_set_flags,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_GET_FLAGS, tegra_gem_get_flags,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_CPU_PREP, tegra_gem_cpu_prep,
			  DRM_UNLOCKED | DRM_RENDER_ALLOW),
#endif
};

static const struct file_operations tegra_drm_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.mmap = tegra_drm_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.compat_ioctl = drm_compat_ioctl,
	.llseek = noop_llseek,
};

static int tegra_drm_context_cleanup(int id, void *p, void *data)
{
	struct tegra_drm_context *context = p;

	tegra_drm_context_free(context);

	return 0;
}

static void tegra_drm_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm *tegra = drm->dev_private;

	mutex_lock(&fpriv->lock);
	idr_for_each(&fpriv->contexts, tegra_drm_context_cleanup, NULL);
	mutex_unlock(&fpriv->lock);

	mutex_lock(&tegra->lock);
	clear_bit(fpriv->drm_context, tegra->drm_contexts);
	mutex_unlock(&tegra->lock);

	idr_destroy(&fpriv->contexts);
	mutex_destroy(&fpriv->lock);
	kfree(fpriv);
}

#ifdef CONFIG_DEBUG_FS
static int tegra_debugfs_framebuffers(struct seq_file *s, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)s->private;
	struct drm_device *drm = node->minor->dev;
	struct drm_framebuffer *fb;

	mutex_lock(&drm->mode_config.fb_lock);

	list_for_each_entry(fb, &drm->mode_config.fb_list, head) {
		seq_printf(s, "%3d: user size: %d x %d, depth %d, %d bpp, refcount %d\n",
			   fb->base.id, fb->width, fb->height,
			   fb->format->depth,
			   fb->format->cpp[0] * 8,
			   drm_framebuffer_read_refcount(fb));
	}

	mutex_unlock(&drm->mode_config.fb_lock);

	return 0;
}

static int tegra_debugfs_iova(struct seq_file *s, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)s->private;
	struct drm_device *drm = node->minor->dev;
	struct tegra_drm *tegra = drm->dev_private;
	struct drm_printer p = drm_seq_file_printer(s);

	if (tegra->domain) {
		mutex_lock(&tegra->mm_lock);
		drm_mm_print(&tegra->mm, &p);
		mutex_unlock(&tegra->mm_lock);
	}

	return 0;
}

static struct drm_info_list tegra_debugfs_list[] = {
	{ "framebuffers", tegra_debugfs_framebuffers, 0 },
	{ "iova", tegra_debugfs_iova, 0 },
};

static int tegra_debugfs_init(struct drm_minor *minor)
{
	return drm_debugfs_create_files(tegra_debugfs_list,
					ARRAY_SIZE(tegra_debugfs_list),
					minor->debugfs_root, minor);
}
#endif

static struct drm_driver tegra_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME |
			   DRIVER_ATOMIC | DRIVER_RENDER,
	.load = tegra_drm_load,
	.unload = tegra_drm_unload,
	.open = tegra_drm_open,
	.postclose = tegra_drm_postclose,
	.lastclose = tegra_drm_lastclose,

#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = tegra_debugfs_init,
#endif

	.gem_free_object_unlocked = tegra_bo_free_object,
	.gem_vm_ops = &tegra_bo_vm_ops,

	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_export = tegra_gem_prime_export,
	.gem_prime_import = tegra_gem_prime_import,

	.dumb_create = tegra_bo_dumb_create,

	.ioctls = tegra_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(tegra_drm_ioctls),
	.fops = &tegra_drm_fops,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

int tegra_drm_register_client(struct tegra_drm *tegra,
			      struct tegra_drm_client *client)
{
	mutex_lock(&tegra->lock);
	list_add_tail(&client->list, &tegra->clients);
	mutex_unlock(&tegra->lock);

	return 0;
}

int tegra_drm_unregister_client(struct tegra_drm *tegra,
				struct tegra_drm_client *client)
{
	mutex_lock(&tegra->lock);
	list_del_init(&client->list);
	mutex_unlock(&tegra->lock);

	return 0;
}

void *tegra_drm_alloc(struct tegra_drm *tegra, size_t size,
			      dma_addr_t *dma)
{
	struct iova *alloc;
	void *virt;
	gfp_t gfp;
	int err;

	if (tegra->carveout)
		size = iova_align(&tegra->carveout->domain, size);
	else
		size = PAGE_ALIGN(size);

	gfp = GFP_KERNEL | __GFP_ZERO;
	if (!tegra->carveout) {
		/*
		 * Many units only support 32-bit addresses, even on 64-bit
		 * SoCs. If there is no IOMMU to translate into a 32-bit IO
		 * virtual address space, force allocations to be in the
		 * lower 32-bit range.
		 */
		gfp |= GFP_DMA;
	}

	virt = (void *)__get_free_pages(gfp, get_order(size));
	if (!virt)
		return ERR_PTR(-ENOMEM);

	if (!tegra->carveout) {
		/*
		 * If IOMMU is disabled, devices address physical memory
		 * directly.
		 */
		*dma = virt_to_phys(virt);
		return virt;
	}

	alloc = alloc_iova(&tegra->carveout->domain,
			   size >> tegra->carveout->shift,
			   tegra->carveout->limit, true);
	if (!alloc) {
		err = -EBUSY;
		goto free_pages;
	}

	*dma = iova_dma_addr(&tegra->carveout->domain, alloc);
	err = iommu_map(tegra->domain, *dma, virt_to_phys(virt),
			size, IOMMU_READ | IOMMU_WRITE);
	if (err < 0)
		goto free_iova;

	return virt;

free_iova:
	__free_iova(&tegra->carveout->domain, alloc);
free_pages:
	free_pages((unsigned long)virt, get_order(size));

	return ERR_PTR(err);
}

void tegra_drm_free(struct tegra_drm *tegra, size_t size, void *virt,
		    dma_addr_t dma)
{
	if (tegra->carveout)
		size = iova_align(&tegra->carveout->domain, size);
	else
		size = PAGE_ALIGN(size);

	if (tegra->carveout) {
		iommu_unmap(tegra->domain, dma, size);
		free_iova(&tegra->carveout->domain,
			  iova_pfn(&tegra->carveout->domain, dma));
	}

	free_pages((unsigned long)virt, get_order(size));
}

static int host1x_drm_probe(struct host1x_device *dev)
{
	struct drm_driver *driver = &tegra_drm_driver;
	struct drm_device *drm;
	int err;

	drm = drm_dev_alloc(driver, &dev->dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	dev_set_drvdata(&dev->dev, drm);

	err = drm_dev_register(drm, 0);
	if (err < 0)
		goto unref;

	return 0;

unref:
	drm_dev_unref(drm);
	return err;
}

static int host1x_drm_remove(struct host1x_device *dev)
{
	struct drm_device *drm = dev_get_drvdata(&dev->dev);

	drm_dev_unregister(drm);
	drm_dev_unref(drm);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int host1x_drm_suspend(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct tegra_drm *tegra = drm->dev_private;

	drm_kms_helper_poll_disable(drm);
	tegra_drm_fb_suspend(drm);

	tegra->state = drm_atomic_helper_suspend(drm);
	if (IS_ERR(tegra->state)) {
		tegra_drm_fb_resume(drm);
		drm_kms_helper_poll_enable(drm);
		return PTR_ERR(tegra->state);
	}

	return 0;
}

static int host1x_drm_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct tegra_drm *tegra = drm->dev_private;

	drm_atomic_helper_resume(drm, tegra->state);
	tegra_drm_fb_resume(drm);
	drm_kms_helper_poll_enable(drm);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(host1x_drm_pm_ops, host1x_drm_suspend,
			 host1x_drm_resume);

static const struct of_device_id host1x_drm_subdevs[] = {
	{ .compatible = "nvidia,tegra20-dc", },
	{ .compatible = "nvidia,tegra20-hdmi", },
	{ .compatible = "nvidia,tegra20-gr2d", },
	{ .compatible = "nvidia,tegra20-gr3d", },
	{ .compatible = "nvidia,tegra30-dc", },
	{ .compatible = "nvidia,tegra30-hdmi", },
	{ .compatible = "nvidia,tegra30-gr2d", },
	{ .compatible = "nvidia,tegra30-gr3d", },
	{ .compatible = "nvidia,tegra114-dsi", },
	{ .compatible = "nvidia,tegra114-hdmi", },
	{ .compatible = "nvidia,tegra114-gr3d", },
	{ .compatible = "nvidia,tegra124-dc", },
	{ .compatible = "nvidia,tegra124-sor", },
	{ .compatible = "nvidia,tegra124-hdmi", },
	{ .compatible = "nvidia,tegra124-dsi", },
	{ .compatible = "nvidia,tegra124-vic", },
	{ .compatible = "nvidia,tegra132-dsi", },
	{ .compatible = "nvidia,tegra210-dc", },
	{ .compatible = "nvidia,tegra210-dsi", },
	{ .compatible = "nvidia,tegra210-sor", },
	{ .compatible = "nvidia,tegra210-sor1", },
	{ .compatible = "nvidia,tegra210-vic", },
	{ .compatible = "nvidia,tegra186-vic", },
	{ /* sentinel */ }
};

static struct host1x_driver host1x_drm_driver = {
	.driver = {
		.name = "drm",
		.pm = &host1x_drm_pm_ops,
	},
	.probe = host1x_drm_probe,
	.remove = host1x_drm_remove,
	.subdevs = host1x_drm_subdevs,
};

static struct platform_driver * const drivers[] = {
	&tegra_dc_driver,
	&tegra_hdmi_driver,
	&tegra_dsi_driver,
	&tegra_dpaux_driver,
	&tegra_sor_driver,
	&tegra_gr2d_driver,
	&tegra_gr3d_driver,
	&tegra_vic_driver,
};

static int __init host1x_drm_init(void)
{
	int err;

	err = host1x_driver_register(&host1x_drm_driver);
	if (err < 0)
		return err;

	err = platform_register_drivers(drivers, ARRAY_SIZE(drivers));
	if (err < 0)
		goto unregister_host1x;

	return 0;

unregister_host1x:
	host1x_driver_unregister(&host1x_drm_driver);
	return err;
}
module_init(host1x_drm_init);

static void __exit host1x_drm_exit(void)
{
	platform_unregister_drivers(drivers, ARRAY_SIZE(drivers));
	host1x_driver_unregister(&host1x_drm_driver);
}
module_exit(host1x_drm_exit);

MODULE_AUTHOR("Thierry Reding <thierry.reding@avionic-design.de>");
MODULE_DESCRIPTION("NVIDIA Tegra DRM driver");
MODULE_LICENSE("GPL v2");
