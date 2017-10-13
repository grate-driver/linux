 
#include "drm.h"

struct tegra_drm_commands_pool;

struct tegra_drm_commands_bo {
	struct tegra_drm_commands_pool *pool;
	struct host1x_bo base;
	dma_addr_t phys;
	dma_addr_t dma;
	void *vaddr;
};

struct tegra_drm_commands_pool *tegra_drm_commands_pool_create(
					struct drm_device *drm,
					size_t block_size,
					unsigned int entries_num,
					unsigned int max_pools_num);

void tegra_drm_commands_pool_destroy(struct tegra_drm_commands_pool *pool);

struct tegra_drm_commands_bo *tegra_drm_commands_pool_alloc(
				struct tegra_drm_commands_pool *pool);

void tegra_drm_commands_pool_free(struct tegra_drm_commands_bo *bo);
