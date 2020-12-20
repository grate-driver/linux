/* SPDX-License-Identifier: GPL-2.0 */

#include "channel.h"
#include "scheduler.h"

struct tegra_drm_channel *
tegra_drm_open_channel(struct tegra_drm *tegra,
		       struct tegra_drm_client *drm_client,
		       u64 pipes_bitmask,
		       unsigned int num_pushbuf_words,
		       unsigned int hw_jobs_limit,
		       unsigned int job_hang_limit,
		       unsigned int timeout_msecs,
		       const char *name)
{
	struct drm_device *drm = tegra->drm;
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct host1x_client *client = &drm_client->base;
	struct tegra_drm_channel *drm_channel;
	int err;

	drm_channel = kzalloc(sizeof(*drm_channel), GFP_KERNEL);
	if (!drm_channel)
		return ERR_PTR(-ENOMEM);

	drm_channel->channel = host1x_channel_request(host, client->dev,
						      num_pushbuf_words);
	if (IS_ERR(drm_channel->channel)) {
		err = PTR_ERR(drm_channel->channel);
		goto err_free_channel;
	}

	drm_channel->acceptable_pipes = pipes_bitmask;

	err = drm_sched_init(&drm_channel->sched,
			     &tegra_drm_sched_ops,
			     hw_jobs_limit, job_hang_limit,
			     msecs_to_jiffies(timeout_msecs / 2),
			     name);
	if (err)
		goto err_put_channel;

	list_add_tail(&drm_channel->list, &tegra->channels);

	return drm_channel;

err_put_channel:
	host1x_channel_put(drm_channel->channel);

err_free_channel:
	kfree(drm_channel);

	return ERR_PTR(err);
}

void tegra_drm_close_channel(struct tegra_drm_channel *drm_channel)
{
	drm_sched_fini(&drm_channel->sched);
	host1x_channel_put(drm_channel->channel);
	list_del(&drm_channel->list);
	kfree(drm_channel);
}
