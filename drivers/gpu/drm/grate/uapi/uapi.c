/*
 * Copyright (C) 2012 Avionic Design GmbH
 * Copyright (C) 2012-2016 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "drm.h"
#include "job.h"
#include "uapi.h"

int tegra_uapi_gem_create(struct drm_device *drm, void *data,
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

int tegra_uapi_gem_mmap(struct drm_device *drm, void *data,
			struct drm_file *file)
{
	struct drm_tegra_gem_mmap *args = data;
	struct drm_gem_object *gem;

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -EINVAL;

	args->offset = drm_vma_node_offset_addr(&gem->vma_node);

	drm_gem_object_put(gem);

	return 0;
}

int tegra_uapi_syncpt_read(struct drm_device *drm, void *data,
			   struct drm_file *file)
{
	return -EPERM;
}

int tegra_uapi_syncpt_incr(struct drm_device *drm, void *data,
			   struct drm_file *file)
{
	return -EPERM;
}

static bool tegra_uapi_v1_done(unsigned int end, unsigned int start)
{
	int diff = end - start;

	return diff >= 0 && diff < INT_MAX;
}

int tegra_uapi_syncpt_wait(struct drm_device *drm, void *data,
			   struct drm_file *file)
{
	struct drm_tegra_syncpt_wait *args = data;
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_drm_context_v1 *context;
	bool scheduled;
	long ret;

	spin_lock(&tegra->context_lock);
	context = idr_find(&fpriv->uapi_v1_contexts, args->id);
	context = tegra_drm_context_v1_get(context);
	spin_unlock(&tegra->context_lock);

	if (!context)
		return -EINVAL;

	scheduled = tegra_uapi_v1_done(context->scheduled_jobs, args->thresh);
	if (!scheduled) {
		if (drm_debug_enabled(DRM_UT_DRIVER))
			DRM_ERROR("invalid fence\n");

		ret = -EINVAL;
		goto put_context;
	}

	ret = wait_event_interruptible_timeout(context->wq,
				tegra_uapi_v1_done(context->completed_jobs,
						   args->thresh),
				msecs_to_jiffies(args->timeout));

put_context:
	tegra_drm_context_v1_put(context);

	if (!ret)
		return -ETIMEDOUT;

	if (ret < 0)
		return ret;

	return 0;
}

int tegra_uapi_open_channel(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct drm_tegra_open_channel *args = data;
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_drm_context_v1 *context;
	struct tegra_drm_client *drm_client;
	bool invalid_class = true;
	int err;

	list_for_each_entry(drm_client, &tegra->clients, list) {
		if (drm_client->base.class == args->client) {
			invalid_class = false;
			break;
		}
	}

	if (invalid_class)
		return -EINVAL;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	kref_init(&context->refcount);
	init_waitqueue_head(&context->wq);
	context->host1x_class = args->client;

	idr_preload(GFP_KERNEL);
	spin_lock(&tegra->context_lock);

	err = idr_alloc(&fpriv->uapi_v1_contexts, context, 1, 0, GFP_ATOMIC);
	context->id = err;

	spin_unlock(&tegra->context_lock);
	idr_preload_end();

	if (err < 0)
		goto err_free_context;

	args->context = err;

	if (tegra->domain)
		args->flags_out |= DRM_TEGRA_CHANNEL_USES_IOMMU;

	return 0;

err_free_context:
	kfree(context);

	return err;
}

void tegra_uapi_v1_free_context(struct tegra_drm_context_v1 *context)
{
	kfree(context);
}

void tegra_uapi_v1_release_context(struct kref *kref)
{
	struct tegra_drm_context_v1 *context;

	context = container_of(kref, struct tegra_drm_context_v1, refcount);
	tegra_uapi_v1_free_context(context);
}

int tegra_uapi_close_channel(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	struct drm_tegra_close_channel *args = data;
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_drm_context_v1 *context;

	spin_lock(&tegra->context_lock);
	context = idr_find(&fpriv->uapi_v1_contexts, args->context);
	if (context)
		idr_remove(&fpriv->uapi_v1_contexts, args->context);
	spin_unlock(&tegra->context_lock);

	if (!context)
		return -EINVAL;

	tegra_drm_context_v1_put(context);

	return 0;
}

int tegra_uapi_get_syncpt(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_tegra_get_syncpt *args = data;
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_drm_context_v1 *context;

	spin_lock(&tegra->context_lock);
	context = idr_find(&fpriv->uapi_v1_contexts, args->context);
	context = tegra_drm_context_v1_get(context);
	spin_unlock(&tegra->context_lock);

	if (!context)
		return -EINVAL;

	args->id = args->context;

	return 0;
}

int tegra_uapi_get_syncpt_base(struct drm_device *drm, void *data,
			       struct drm_file *file)
{
	return -EPERM;
}

int tegra_uapi_gem_set_tiling(struct drm_device *drm, void *data,
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

	drm_gem_object_put(gem);

	return 0;
}

int tegra_uapi_gem_get_tiling(struct drm_device *drm, void *data,
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

	drm_gem_object_put(gem);

	return err;
}

int tegra_uapi_gem_set_flags(struct drm_device *drm, void *data,
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

	drm_gem_object_put(gem);

	return 0;
}

int tegra_uapi_gem_get_flags(struct drm_device *drm, void *data,
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

	if (bo->sgt->nents > 1)
		args->flags |= DRM_TEGRA_GEM_SPARSE;

	drm_gem_object_put(gem);

	return 0;
}

int tegra_uapi_v1_submit(struct drm_device *drm, void *data,
			 struct drm_file *file)
{
	struct drm_tegra_submit *submit = data;
	int err;

	err = tegra_drm_submit_job_v1(drm, submit, file);
	if (err)
		return err;

	return 0;
}

int tegra_uapi_v2_submit(struct drm_device *drm, void *data,
			 struct drm_file *file)
{
	struct drm_tegra_submit_v2 *submit = data;
	int err;

	if (submit->uapi_ver > GRATE_KERNEL_DRM_VERSION) {
		DRM_ERROR("unsupported uapi version %u, maximum is %u\n",
			  submit->uapi_ver, GRATE_KERNEL_DRM_VERSION);
		return -EINVAL;
	}

	err = tegra_drm_submit_job_v2(drm, submit, file);
	if (err)
		return err;

	return 0;
}

int tegra_uapi_gem_cpu_prep(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct drm_tegra_gem_cpu_prep *args = data;
	struct drm_gem_object *gem;
	struct tegra_bo *bo;
	unsigned long timeout;
	bool write;
	int ret;

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem) {
		DRM_ERROR("failed to find bo handle %u\n", args->handle);
		return -ENOENT;
	}

	bo = to_tegra_bo(gem);
	write = !!(args->flags & DRM_TEGRA_CPU_PREP_WRITE);
	timeout = usecs_to_jiffies(args->timeout);

	if (timeout)
		ret = dma_resv_wait_timeout_rcu(bo->gem.resv, write, true,
						timeout);
	else
		ret = dma_resv_test_signaled_rcu(bo->gem.resv, write);

	drm_gem_object_put(gem);

	if (ret == 0) {
		DRM_DEBUG_DRIVER("bo handle %u is busy\n", args->handle);
		return timeout == 0 ? -EBUSY : -ETIMEDOUT;
	}

	if (ret < 0) {
		if (ret != -ERESTARTSYS || drm_debug_enabled(DRM_UT_DRIVER))
			DRM_ERROR("failed to await bo handle %u: %d\n",
				  args->handle, ret);
		return ret;
	}

	DRM_DEBUG_DRIVER("bo handle %u is idling\n", args->handle);

	return 0;
}

int tegra_uapi_version(struct drm_device *drm, void *data,
		       struct drm_file *file)
{
	struct drm_tegra_version *args = data;

	if (of_machine_is_compatible("nvidia,tegra20"))
		args->soc_ver = DRM_TEGRA_SOC_T20;
	else if (of_machine_is_compatible("nvidia,tegra30"))
		args->soc_ver = DRM_TEGRA_SOC_T30;
	else if (of_machine_is_compatible("nvidia,tegra114"))
		args->soc_ver = DRM_TEGRA_SOC_T114;
	else if (of_machine_is_compatible("nvidia,tegra124"))
		args->soc_ver = DRM_TEGRA_SOC_T124;
	else if (of_machine_is_compatible("nvidia,tegra132"))
		args->soc_ver = DRM_TEGRA_SOC_T132;
	else if (of_machine_is_compatible("nvidia,tegra148"))
		args->soc_ver = DRM_TEGRA_SOC_T148;
	else if (of_machine_is_compatible("nvidia,tegra210"))
		args->soc_ver = DRM_TEGRA_SOC_T210;
	else if (of_machine_is_compatible("nvidia,tegra186"))
		args->soc_ver = DRM_TEGRA_SOC_T186;
	else if (of_machine_is_compatible("nvidia,tegra194"))
		args->soc_ver = DRM_TEGRA_SOC_T194;
	else
		return -EINVAL;

	args->uapi_ver = GRATE_KERNEL_DRM_VERSION;

	return 0;
}
