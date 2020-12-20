/* SPDX-License-Identifier: GPL-2.0 */

#include "host1x.h"

static int host1x_alloc_phys_memory(struct host1x *host,
				    struct host1x_alloc_desc *desc)
{
	/*
	 * Note that on ARM32 we're always avoiding the implicit backing and
	 * the "addr" is the PHYS address, on ARM64 it is DMA address if IOMMU
	 * domain type is IOMMU_DOMAIN_DMA.
	 */
	desc->vaddr = dma_alloc_attrs(host->dev, desc->size, &desc->addr,
				      GFP_KERNEL | __GFP_NOWARN,
				      desc->dma_attrs);
	if (!desc->vaddr)
		return -ENOMEM;

	return 0;
}

static void host1x_free_phys_memory(struct host1x *host,
				    struct host1x_alloc_desc *desc)
{
	dma_free_attrs(host->dev, desc->size, desc->vaddr, desc->addr,
		       desc->dma_attrs);
}

int host1x_alloc_memory(struct host1x *host,
			struct host1x_alloc_desc *desc)
{
	int err;

	/* allocate a chunk of memory */
	err = host1x_alloc_phys_memory(host, desc);
	if (err)
		return err;

	/* map that chunk into the HOST1x's address space */
	err = host1x_iommu_map_memory(host, desc);
	if (err)
		goto err_free_phys_memory;

	return 0;

err_free_phys_memory:
	host1x_free_phys_memory(host, desc);

	return err;
}

void host1x_free_memory(struct host1x *host,
			struct host1x_alloc_desc *desc)
{
	host1x_iommu_unmap_memory(host, desc);
	host1x_free_phys_memory(host, desc);
}

int host1x_bo_alloc_standalone_data(struct host1x *host,
				    struct host1x_bo *bo,
				    size_t size)
{
	struct host1x_alloc_desc desc;
	int err;

	if (host->domain)
		desc.size = iova_align(&host->iova, size);
	else
		desc.size = PAGE_ALIGN(size);

	desc.dma_attrs = DMA_ATTR_WRITE_COMBINE;

	err = host1x_alloc_memory(host, &desc);
	if (err)
		return err;

	bo->dmaaddr	= desc.dmaaddr;
	bo->addr	= desc.addr;
	bo->vaddr	= desc.vaddr;
	bo->size	= desc.size;
	bo->dma_attrs	= desc.dma_attrs;
	bo->from_pool	= false;

	return 0;
}
EXPORT_SYMBOL(host1x_bo_alloc_standalone_data);

void host1x_bo_free_standalone_data(struct host1x *host,
				    struct host1x_bo *bo)
{
	struct host1x_alloc_desc desc;

	desc.dmaaddr	= bo->dmaaddr;
	desc.addr	= bo->addr;
	desc.vaddr	= bo->vaddr;
	desc.dma_attrs	= bo->dma_attrs;
	desc.size	= bo->size;

	host1x_free_memory(host, &desc);
}
EXPORT_SYMBOL(host1x_bo_free_standalone_data);
