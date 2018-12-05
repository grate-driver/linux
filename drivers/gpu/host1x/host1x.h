/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __HOST1X_H
#define __HOST1X_H

#include <linux/host1x.h>

struct host1x_alloc_desc {
	struct iova *iova;
	dma_addr_t dmaaddr;
	dma_addr_t addr;
	void *vaddr;
	size_t size;
	ulong dma_attrs;
};

int host1x_alloc_memory(struct host1x *host,
			struct host1x_alloc_desc *desc);
void host1x_free_memory(struct host1x *host,
			struct host1x_alloc_desc *desc);

int host1x_iommu_map_memory(struct host1x *host,
			    struct host1x_alloc_desc *desc);
void host1x_iommu_unmap_memory(struct host1x *host,
			       struct host1x_alloc_desc *desc);

int host1x_init_iommu(struct host1x *host);
void host1x_deinit_iommu(struct host1x *host);

int host1x_init_dma_pool(struct host1x *host);
void host1x_deinit_dma_pool(struct host1x *host);

static inline int
host1x_init_syncpts(struct host1x *host)
{
	return host->syncpt_ops.init(host);
}

static inline void
host1x_deinit_syncpts(struct host1x *host)
{
	host->syncpt_ops.deinit(host);
}

static inline int
host1x_init_channels(struct host1x *host)
{
	return host->chan_ops.init(host);
}

static inline void
host1x_deinit_channels(struct host1x *host)
{
	host->chan_ops.deinit(host);
}

static inline int
host1x_init_mlocks(struct host1x *host)
{
	return host->mlock_ops.init(host);
}

static inline void
host1x_deinit_mlocks(struct host1x *host)
{
	host->mlock_ops.deinit(host);
}

int host1x_init_debug(struct host1x *host);
void host1x_deinit_debug(struct host1x *host);

extern struct platform_driver tegra_mipi_driver;

#endif
