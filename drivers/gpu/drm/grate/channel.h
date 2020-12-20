/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __TEGRA_DRM_CHANNEL_H
#define __TEGRA_DRM_CHANNEL_H

#include "drm.h"

#define TEGRA_DRM_PIPE_2D	BIT(DRM_TEGRA_PIPE_ID_2D)
#define TEGRA_DRM_PIPE_3D	BIT(DRM_TEGRA_PIPE_ID_3D)
#define TEGRA_DRM_PIPE_VIC	BIT(DRM_TEGRA_PIPE_ID_VIC)

struct tegra_drm_channel {
	struct drm_gpu_scheduler sched;
	struct host1x_channel *channel;
	struct list_head list;
	u64 acceptable_pipes;
};

static inline struct tegra_drm_channel *
to_tegra_drm_channel(struct drm_gpu_scheduler *sched)
{
	return container_of(sched, struct tegra_drm_channel, sched);
}

struct tegra_drm;
struct tegra_drm_client;

struct tegra_drm_channel *
tegra_drm_open_channel(struct tegra_drm *tegra,
		       struct tegra_drm_client *drm_client,
		       u64 pipes_bitmask,
		       unsigned int num_pushbuf_words,
		       unsigned int hw_jobs_limit,
		       unsigned int job_hang_limit,
		       unsigned int timeout_msecs,
		       const char *name);

void tegra_drm_close_channel(struct tegra_drm_channel *drm_channel);

#endif
