/*
 * Copyright (C) 2012 Avionic Design GmbH
 * Copyright (C) 2012-2016 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __TEGRA_DRM_UAPI_H
#define __TEGRA_DRM_UAPI_H

#include <linux/wait.h>

#include "drm.h"

struct tegra_drm_context_v1 {
	unsigned int host1x_class;
	struct wait_queue_head wq;
	struct kref refcount;
	u32 completed_jobs;
	u32 scheduled_jobs;
	unsigned int id;
};

void tegra_uapi_v1_free_context(struct tegra_drm_context_v1 *context);
void tegra_uapi_v1_release_context(struct kref *kref);

static inline struct tegra_drm_context_v1 *
tegra_uapi_context_v1_find(struct tegra_drm *tegra,
			   struct tegra_drm_file *fpriv,
			   u32 id)
{
	struct tegra_drm_context_v1 *context;

	spin_lock(&tegra->context_lock);

	context = idr_find(&fpriv->uapi_v1_contexts, id);
	if (context)
		kref_get(&context->refcount);

	spin_unlock(&tegra->context_lock);

	return context;
}

static inline struct tegra_drm_context_v1 *
tegra_drm_context_v1_get(struct tegra_drm_context_v1 *context)
{
	if (context)
		kref_get(&context->refcount);

	return context;
}

static inline void
tegra_drm_context_v1_put(struct tegra_drm_context_v1 *context)
{
	kref_put(&context->refcount, tegra_uapi_v1_release_context);
}

int tegra_uapi_gem_create(struct drm_device *drm, void *data,
			  struct drm_file *file);

int tegra_uapi_gem_mmap(struct drm_device *drm, void *data,
			struct drm_file *file);

int tegra_uapi_syncpt_read(struct drm_device *drm, void *data,
			   struct drm_file *file);

int tegra_uapi_syncpt_incr(struct drm_device *drm, void *data,
			   struct drm_file *file);

int tegra_uapi_syncpt_wait(struct drm_device *drm, void *data,
			   struct drm_file *file);

int tegra_uapi_open_channel(struct drm_device *drm, void *data,
			    struct drm_file *file);

int tegra_uapi_close_channel(struct drm_device *drm, void *data,
			     struct drm_file *file);

int tegra_uapi_get_syncpt(struct drm_device *drm, void *data,
			  struct drm_file *file);

int tegra_uapi_get_syncpt_base(struct drm_device *drm, void *data,
			       struct drm_file *file);

int tegra_uapi_gem_set_tiling(struct drm_device *drm, void *data,
			      struct drm_file *file);

int tegra_uapi_gem_get_tiling(struct drm_device *drm, void *data,
			      struct drm_file *file);

int tegra_uapi_gem_set_flags(struct drm_device *drm, void *data,
			     struct drm_file *file);

int tegra_uapi_gem_get_flags(struct drm_device *drm, void *data,
			     struct drm_file *file);

int tegra_uapi_v1_submit(struct drm_device *drm, void *data,
			 struct drm_file *file);

int tegra_uapi_v2_submit(struct drm_device *drm, void *data,
			 struct drm_file *file);

int tegra_uapi_gem_cpu_prep(struct drm_device *drm, void *data,
			    struct drm_file *file);

int tegra_uapi_version(struct drm_device *drm, void *data,
		       struct drm_file *file);

#endif
