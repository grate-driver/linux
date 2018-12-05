/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/platform_device.h>

#if IS_ENABLED(CONFIG_ARM_DMA_USE_IOMMU)
#include <asm/dma-iommu.h>
#endif

#include "host1x.h"

static void host1x_setup_sid_table(struct host1x *host)
{
	const struct host1x_soc *soc = host->soc;
	unsigned int i;

	for (i = 0; i < soc->nb_sid_entries; i++) {
		const struct host1x_sid_entry *entry = &soc->sid_table[i];

		writel_relaxed(entry->offset, host->hv_regs + entry->base);
		writel_relaxed(entry->limit, host->hv_regs + entry->base + 4);
	}
}

int host1x_init_iommu(struct host1x *host)
{
	struct iommu_domain_geometry *geometry;
	struct iommu_domain *domain;
	u64 mask = dma_get_mask(host->dev);
	dma_addr_t start, end;
	unsigned long order;
	int err;

#if IS_ENABLED(CONFIG_ARM_DMA_USE_IOMMU)
	if (host->dev->archdata.mapping) {
		struct dma_iommu_mapping *mapping =
				to_dma_iommu_mapping(host->dev);
		arm_iommu_detach_device(host->dev);
		arm_iommu_release_mapping(mapping);
	}
#endif
	domain = iommu_get_domain_for_dev(host->dev);

	/* DMA API manages IOVA mappings for us */
	if (domain && domain->type == IOMMU_DOMAIN_DMA)
		return 0;

	host->group = iommu_group_get(host->dev);
	if (!host->group)
		return 0;

	err = iova_cache_get();
	if (err)
		goto put_group;

	host->domain = iommu_domain_alloc(&platform_bus_type);
	if (!host->domain) {
		err = -ENOMEM;
		goto put_cache;
	}

	err = iommu_attach_group(host->domain, host->group);
	if (err)
		goto free_domain;

	geometry = &host->domain->geometry;
	start = geometry->aperture_start & mask;
	end = geometry->aperture_end & mask;

	order = __ffs(host->domain->pgsize_bitmap);
	init_iova_domain(&host->iova, 1UL << order, start >> order);
	host->iova_end = end;

	host1x_setup_sid_table(host);

	return 0;

free_domain:
	iommu_domain_free(host->domain);
put_cache:
	iova_cache_put();
put_group:
	iommu_group_put(host->group);

	host->domain = NULL;

	return err;
}

void host1x_deinit_iommu(struct host1x *host)
{
	if (host->domain) {
		put_iova_domain(&host->iova);
		iommu_detach_group(host->domain, host->group);
		iommu_domain_free(host->domain);
		iova_cache_put();
		iommu_group_put(host->group);
	}
}

int host1x_iommu_map_memory(struct host1x *host,
			    struct host1x_alloc_desc *desc)
{
	unsigned long shift;
	struct iova *alloc;
	int err;

	if (host->domain) {
		shift = iova_shift(&host->iova);

		alloc = alloc_iova(&host->iova,
				   desc->size >> shift,
				   host->iova_end >> shift,
				   true);
		if (!alloc)
			return -ENOMEM;

		desc->dmaaddr = iova_dma_addr(&host->iova, alloc);

		err = iommu_map(host->domain, desc->dmaaddr, desc->addr,
				desc->size, IOMMU_READ);
		if (err) {
			__free_iova(&host->iova, alloc);
			return err;
		}
	} else {
		desc->dmaaddr = desc->addr;
	}

	return 0;
}

void host1x_iommu_unmap_memory(struct host1x *host,
			       struct host1x_alloc_desc *desc)
{
	if (host->domain) {
		iommu_unmap(host->domain, desc->dmaaddr, desc->size);
		free_iova(&host->iova, iova_pfn(&host->iova, desc->dmaaddr));
	}
}
