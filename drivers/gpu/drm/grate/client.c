/* SPDX-License-Identifier: GPL-2.0 */

#include "drm.h"

#if IS_ENABLED(CONFIG_ARM_DMA_USE_IOMMU)
#include <asm/dma-iommu.h>
#endif

int tegra_drm_register_client(struct tegra_drm *tegra,
			      struct tegra_drm_client *drm_client)
{
	struct drm_device *drm = tegra->drm;
	struct host1x *host1x = dev_get_drvdata(drm->dev->parent);
	struct host1x_client *client = &drm_client->base;
	int err;

	drm_client->mlock = host1x_mlock_request(host1x, client->dev);
	if (IS_ERR(drm_client->mlock)) {
		err = PTR_ERR(drm_client->mlock);
		return err;
	}

	list_add_tail(&drm_client->list, &tegra->clients);
	drm_client->drm = tegra;

	return 0;
}

void tegra_drm_unregister_client(struct tegra_drm_client *drm_client)
{
	host1x_mlock_put(drm_client->mlock);
	list_del(&drm_client->list);
	drm_client->drm = NULL;
}

struct iommu_group *
tegra_drm_client_iommu_attach(struct tegra_drm_client *drm_client, bool shared)
{
	struct host1x_client *client = &drm_client->base;
	struct drm_device *drm = dev_get_drvdata(client->host);
	struct tegra_drm *tegra = drm->dev_private;
	struct iommu_group *group = NULL;
	int err;

	if (tegra->domain) {
		group = iommu_group_get(client->dev);
		if (!group) {
			dev_err(client->dev, "failed to get IOMMU group\n");
			return ERR_PTR(-ENODEV);
		}

		if (!shared || !tegra->group) {
#if IS_ENABLED(CONFIG_ARM_DMA_USE_IOMMU)
			if (client->dev->archdata.mapping) {
				struct dma_iommu_mapping *mapping =
					to_dma_iommu_mapping(client->dev);
				arm_iommu_detach_device(client->dev);
				arm_iommu_release_mapping(mapping);
			}
#endif
			err = iommu_attach_group(tegra->domain, group);
			if (err < 0) {
				iommu_group_put(group);
				return ERR_PTR(err);
			}

			if (shared)
				tegra->group = group;
		}
	}

	return group;
}

void tegra_drm_client_iommu_detach(struct tegra_drm_client *drm_client,
				   struct iommu_group *group,
				   bool shared)
{
	struct host1x_client *client = &drm_client->base;
	struct drm_device *drm = dev_get_drvdata(client->host);
	struct tegra_drm *tegra = drm->dev_private;

	if (group) {
		if (!shared || group == tegra->group) {
			iommu_detach_group(tegra->domain, group);

			if (group == tegra->group)
				tegra->group = NULL;
		}

		iommu_group_put(group);
	}
}
