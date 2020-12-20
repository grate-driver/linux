/* SPDX-License-Identifier: GPL-2.0 */

#include "debug.h"

static void write_to_printk(const char *str, size_t len, bool cont,
			    void *opaque)
{
	if (!drm_debug_enabled(DRM_UT_DRIVER))
		return;

	if (cont)
		pr_cont("%s", str);
	else
		DRM_DEBUG_DRIVER("%s", str);
}

static struct host1x_dbg_output tegra_drm_dbg = {
	.fn = write_to_printk,
};

void tegra_drm_debug_dump_hung_job(struct tegra_drm_job *drm_job)
{
	struct tegra_drm_channel *drm_channel = drm_job->drm_channel;
	struct host1x_channel *chan = drm_channel->channel;
	struct host1x_job *job = &drm_job->base;
	struct host1x *host = drm_job->host;

	host1x_debug_output_lock(host);
	host1x_debug_dump_channel(host, &tegra_drm_dbg, chan);
	host1x_debug_dump_job(host, &tegra_drm_dbg, job);
	host1x_debug_dump_syncpt(host, &tegra_drm_dbg, job->syncpt);
	host1x_debug_dump_channels_pushbuf(host, &tegra_drm_dbg, chan);
	host1x_debug_dump_mlocks(host, &tegra_drm_dbg);
	host1x_debug_output_unlock(host);
}

void tegra_drm_debug_dump_job(struct tegra_drm_job *drm_job)
{
	struct host1x_job *job = &drm_job->base;
	struct host1x *host = drm_job->host;

	host1x_debug_output_lock(host);
	host1x_debug_dump_job(host, &tegra_drm_dbg, job);
	host1x_debug_output_unlock(host);
}
