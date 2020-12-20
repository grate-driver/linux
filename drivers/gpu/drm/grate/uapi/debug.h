/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __TEGRA_DRM_UAPI_DEBUG_H
#define __TEGRA_DRM_UAPI_DEBUG_H

#include "drm.h"
#include "job.h"

void tegra_drm_debug_dump_hung_job(struct tegra_drm_job *drm_job);
void tegra_drm_debug_dump_job(struct tegra_drm_job *drm_job);

#endif
