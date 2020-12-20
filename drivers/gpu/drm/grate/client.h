/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __TEGRA_DRM_CLIENT_H
#define __TEGRA_DRM_CLIENT_H

#include "drm.h"

struct tegra_drm_job;

struct tegra_drm_client {
	struct host1x_client base;
	struct host1x_mlock *mlock;
	struct tegra_drm *drm;
	struct list_head list;
	const unsigned long *addr_regs;
	unsigned int num_regs;
	u64 pipe;

	int (*refine_class)(struct tegra_drm_client *client, u64 pipes,
			    unsigned int *classid);

	int (*prepare_job)(struct tegra_drm_client *client,
			   struct tegra_drm_job *job);

	int (*unprepare_job)(struct tegra_drm_client *client,
			     struct tegra_drm_job *job);

	int (*reset_hw)(struct tegra_drm_client *client);
};

static inline struct tegra_drm_client *
to_tegra_drm_client(struct host1x_client *client)
{
	return container_of(client, struct tegra_drm_client, base);
}

struct tegra_drm;

int tegra_drm_register_client(struct tegra_drm *tegra,
			      struct tegra_drm_client *drm_client);

void tegra_drm_unregister_client(struct tegra_drm_client *drm_client);

struct iommu_group *
tegra_drm_client_iommu_attach(struct tegra_drm_client *drm_client, bool shared);

void tegra_drm_client_iommu_detach(struct tegra_drm_client *drm_client,
				   struct iommu_group *group,
				   bool shared);

#endif
