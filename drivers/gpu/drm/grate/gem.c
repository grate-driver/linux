// SPDX-License-Identifier: GPL-2.0-only
/*
 * NVIDIA Tegra DRM GEM helper functions
 *
 * Copyright (C) 2012 Sascha Hauer, Pengutronix
 * Copyright (C) 2013-2015 NVIDIA CORPORATION, All rights reserved.
 *
 * Based on the GEM/CMA helpers
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 */

#include <linux/dma-buf.h>
#include <linux/iommu.h>

#include <drm/drm_drv.h>
#include <drm/drm_prime.h>

#include "drm.h"
#include "gem.h"
#include "gart.h"

static int tegra_bo_iommu_map(struct tegra_drm *tegra, struct tegra_bo *bo)
{
	unsigned long order = __ffs(tegra->domain->pgsize_bitmap);
	int prot = IOMMU_READ | IOMMU_WRITE;
	size_t iosize;
	int err;

	mutex_lock(&tegra->mm_lock);

	if (drm_mm_node_allocated(&bo->mm)) {
		err = -EBUSY;
		goto unlock;
	}

	/*
	 * Only Tegra20 has GART and in that case mappings are done by
	 * uapi/gart.c, consult the code for more details.
	 */
	if (IS_ENABLED(CONFIG_TEGRA_IOMMU_GART) && tegra->has_gart) {
		err = 0;
		goto unlock;
	}

	err = drm_mm_insert_node_generic(&tegra->mm, &bo->mm, bo->gem.size,
					 1UL << order, 0, 0);
	if (err < 0) {
		dev_err(tegra->drm->dev, "out of I/O virtual memory: %d\n",
			err);
		goto unlock;
	}

	bo->dmaaddr = bo->mm.start;

	iosize = iommu_map_sgtable(tegra->domain, bo->dmaaddr, bo->sgt, prot);
	if (iosize != bo->gem.size) {
		dev_err(tegra->drm->dev, "failed to map buffer\n");
		err = -ENOMEM;
		goto remove;
	}

	mutex_unlock(&tegra->mm_lock);

	return 0;

remove:
	drm_mm_remove_node(&bo->mm);
unlock:
	mutex_unlock(&tegra->mm_lock);
	return err;
}

static int tegra_bo_iommu_unmap(struct tegra_drm *tegra, struct tegra_bo *bo)
{
	mutex_lock(&tegra->mm_lock);

	if (!drm_mm_node_allocated(&bo->mm))
		goto unlock;

	if (IS_ENABLED(CONFIG_TEGRA_IOMMU_GART) && tegra->has_gart) {
		tegra_bo_gart_unmap_locked(tegra, bo);
	} else {
		iommu_unmap(tegra->domain, bo->dmaaddr, bo->gem.size);
		drm_mm_remove_node(&bo->mm);
	}
unlock:
	mutex_unlock(&tegra->mm_lock);

	return 0;
}

static const struct drm_gem_object_funcs tegra_gem_object_funcs = {
	.free = tegra_bo_free_object,
	.export = tegra_gem_prime_export,
	.vm_ops = &tegra_bo_vm_ops,
};

static struct tegra_bo *tegra_bo_alloc_object(struct drm_device *drm,
					      struct dma_resv *resv,
					      size_t size)
{
	struct tegra_bo *bo;
	int err;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&bo->mm_eviction_entry);

	bo->gem.resv	= resv;

	/* memory controller traps these addresses on all Tegra SoCs */
	bo->gartaddr	= TEGRA_POISON_ADDR;
	bo->dmaaddr	= TEGRA_POISON_ADDR;
	bo->paddr	= TEGRA_POISON_ADDR;

	bo->gem.funcs = &tegra_gem_object_funcs;

	size = round_up(size, PAGE_SIZE);

	err = drm_gem_object_init(drm, &bo->gem, size);
	if (err < 0)
		goto free;

	err = drm_gem_create_mmap_offset(&bo->gem);
	if (err < 0)
		goto release;

	return bo;

release:
	drm_gem_object_release(&bo->gem);
free:
	kfree(bo);
	return ERR_PTR(err);
}

static void tegra_bo_free(struct drm_device *drm, struct tegra_bo *bo)
{
	struct host1x *host = dev_get_drvdata(drm->dev->parent);

	if (bo->flags & TEGRA_BO_HOST1X_GATHER) {
		host1x_bo_free(host, bo->host1x_bo);
	} else if (bo->pages) {
		dma_unmap_sgtable(drm->dev, bo->sgt, DMA_FROM_DEVICE, 0);
		drm_gem_put_pages(&bo->gem, bo->pages, true, true);
	} else if (bo->dma_cookie) {
		dma_free_attrs(drm->dev, bo->gem.size, bo->dma_cookie,
			       bo->paddr, bo->dma_attrs);
	}

	if (bo->sgt) {
		sg_free_table(bo->sgt);
		kfree(bo->sgt);
	}
}

static int tegra_bo_get_pages(struct drm_device *drm, struct tegra_bo *bo)
{
	int err;

	bo->pages = drm_gem_get_pages(&bo->gem);
	if (IS_ERR(bo->pages))
		return PTR_ERR(bo->pages);

	bo->num_pages = bo->gem.size >> PAGE_SHIFT;

	bo->sgt = drm_prime_pages_to_sg(bo->gem.dev, bo->pages, bo->num_pages);
	if (IS_ERR(bo->sgt)) {
		err = PTR_ERR(bo->sgt);
		goto put_pages;
	}

	err = dma_map_sgtable(drm->dev, bo->sgt, DMA_FROM_DEVICE, 0);
	if (err)
		goto free_sgt;

	return 0;

free_sgt:
	sg_free_table(bo->sgt);
	kfree(bo->sgt);
put_pages:
	drm_gem_put_pages(&bo->gem, bo->pages, false, false);
	return err;
}

static int tegra_bo_alloc(struct drm_device *drm, struct tegra_bo *bo,
			  unsigned long drm_flags)
{
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct tegra_drm *tegra = drm->dev_private;
	unsigned long dma_attrs;
	bool from_pool = false;
	bool want_sparse;
	int err;

	if (drm_flags & DRM_TEGRA_GEM_CREATE_CONTIGUOUS)
		want_sparse = false;
	else if (drm_flags & DRM_TEGRA_GEM_CREATE_SPARSE)
		want_sparse = true;
	else if (IS_ENABLED(CONFIG_TEGRA_IOMMU_GART) && tegra->has_gart)
		want_sparse = false;
	else
		want_sparse = true;

	if (bo->flags & TEGRA_BO_HOST1X_GATHER) {
		bo->host1x_bo = host1x_bo_alloc(host, bo->gem.size,
						from_pool);
		if (!bo->host1x_bo)
			return -ENOMEM;

		bo->vaddr = bo->host1x_bo->vaddr;
		bo->dmaaddr = bo->host1x_bo->dmaaddr;

	} else if (tegra->domain && want_sparse) {
		err = tegra_bo_get_pages(drm, bo);
		if (err < 0)
			return err;

		err = tegra_bo_iommu_map(tegra, bo);
		if (err < 0) {
			tegra_bo_free(drm, bo);
			return err;
		}

		if (IS_ENABLED(CONFIG_TEGRA_IOMMU_GART) &&
		    tegra->has_gart && bo->sgt->nents == 1)
			bo->dmaaddr = sg_dma_address(bo->sgt->sgl);
	} else {
		size_t size = bo->gem.size;

		dma_attrs = DMA_ATTR_WRITE_COMBINE |
			    DMA_ATTR_FORCE_CONTIGUOUS;

		if (drm_flags & DRM_TEGRA_GEM_CREATE_DONT_KMAP)
			dma_attrs |= DMA_ATTR_NO_KERNEL_MAPPING;

		bo->dma_cookie = dma_alloc_attrs(drm->dev, size,
						 &bo->paddr, GFP_KERNEL,
						 dma_attrs | DMA_ATTR_NO_WARN);
		if (!bo->dma_cookie)
			return -ENOMEM;

		bo->dma_attrs = dma_attrs;

		if (dma_attrs & DMA_ATTR_NO_KERNEL_MAPPING)
			bo->vaddr = NULL;
		else
			bo->vaddr = bo->dma_cookie;

		bo->sgt = kmalloc(sizeof(*bo->sgt), GFP_KERNEL);
		if (!bo->sgt) {
			dma_free_attrs(drm->dev, size, bo->dma_cookie,
				       bo->paddr, dma_attrs);
			return -ENOMEM;
		}

		err = dma_get_sgtable(drm->dev, bo->sgt, bo->dma_cookie,
				      bo->paddr, size);
		if (err < 0) {
			dma_free_attrs(drm->dev, size, bo->dma_cookie,
				       bo->paddr, dma_attrs);
			kfree(bo->sgt);
			return err;
		}

		if (tegra->domain) {
			err = tegra_bo_iommu_map(tegra, bo);
			if (err < 0) {
				tegra_bo_free(drm, bo);
				return err;
			}

			if (IS_ENABLED(CONFIG_TEGRA_IOMMU_GART) &&
			    tegra->has_gart)
				bo->dmaaddr = bo->paddr;
		} else {
			bo->dmaaddr = bo->paddr;
		}
	}

	return 0;
}

struct tegra_bo *tegra_bo_create(struct drm_device *drm, size_t size,
				 unsigned long drm_flags, bool want_kmap)
{
	struct tegra_bo *bo;
	int err;

	bo = tegra_bo_alloc_object(drm, NULL, size);
	if (IS_ERR(bo))
		return bo;

	if (drm_flags & DRM_TEGRA_GEM_CREATE_TILED)
		bo->tiling.mode = TEGRA_BO_TILING_MODE_TILED;

	if (drm_flags & DRM_TEGRA_GEM_CREATE_BOTTOM_UP)
		bo->flags |= TEGRA_BO_BOTTOM_UP;

	if (drm_flags & DRM_TEGRA_GEM_CREATE_HOST1X_GATHER)
		bo->flags |= TEGRA_BO_HOST1X_GATHER;

	/*
	 * UAPI v2 users always want to set the DONT_KMAP flags.
	 * UAPI v1 users not, for them the only purpose of KMAP'ing
	 * is to copy commands buffer data during jobs submission
	 * and hence we can safely assume that large buffer doesn't
	 * need the mapping.
	 *
	 * Note that kernel's framebuffer need to have the mapping.
	 */
	if (!want_kmap && size > SZ_256K)
		drm_flags |= DRM_TEGRA_GEM_CREATE_DONT_KMAP;

	err = tegra_bo_alloc(drm, bo, drm_flags);
	if (err < 0) {
		dev_err(drm->dev, "failed to allocate buffer of size %zu: %d\n",
			size, err);
		goto release;
	}

	return bo;

release:
	drm_gem_object_release(&bo->gem);
	kfree(bo);
	return ERR_PTR(err);
}

struct tegra_bo *tegra_bo_create_with_handle(struct drm_file *file,
					     struct drm_device *drm,
					     size_t size,
					     unsigned long drm_flags,
					     u32 *handle)
{
	struct tegra_bo *bo;
	int err;

	bo = tegra_bo_create(drm, size, drm_flags, false);
	if (IS_ERR(bo))
		return bo;

	err = drm_gem_handle_create(file, &bo->gem, handle);
	if (err) {
		tegra_bo_free_object(&bo->gem);
		return ERR_PTR(err);
	}

	drm_gem_object_put(&bo->gem);

	return bo;
}

static struct tegra_bo *tegra_bo_import(struct drm_device *drm,
					struct dma_buf *buf)
{
	struct tegra_drm *tegra = drm->dev_private;
	struct dma_buf_attachment *attach;
	struct tegra_bo *bo;
	int err;

	bo = tegra_bo_alloc_object(drm, buf->resv, buf->size);
	if (IS_ERR(bo))
		return bo;

	attach = dma_buf_attach(buf, drm->dev);
	if (IS_ERR(attach)) {
		err = PTR_ERR(attach);
		goto free;
	}

	get_dma_buf(buf);

	bo->sgt = dma_buf_map_attachment(attach, DMA_TO_DEVICE);
	if (IS_ERR(bo->sgt)) {
		err = PTR_ERR(bo->sgt);
		goto detach;
	}

	if (tegra->domain) {
		err = tegra_bo_iommu_map(tegra, bo);
		if (err < 0)
			goto detach;

		if (IS_ENABLED(CONFIG_TEGRA_IOMMU_GART) && tegra->has_gart &&
		    bo->sgt->nents == 1)
			bo->dmaaddr = sg_dma_address(bo->sgt->sgl);
	} else {
		if (bo->sgt->nents > 1) {
			err = -EINVAL;
			goto detach;
		}

		bo->dmaaddr = sg_dma_address(bo->sgt->sgl);
	}

	bo->gem.import_attach = attach;

	return bo;

detach:
	if (!IS_ERR_OR_NULL(bo->sgt))
		dma_buf_unmap_attachment(attach, bo->sgt, DMA_TO_DEVICE);

	dma_buf_detach(buf, attach);
	dma_buf_put(buf);
free:
	drm_gem_object_release(&bo->gem);
	kfree(bo);
	return ERR_PTR(err);
}

void tegra_bo_free_object(struct drm_gem_object *gem)
{
	struct drm_device *drm = gem->dev;
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_bo *bo = to_tegra_bo(gem);

	if (tegra->domain)
		tegra_bo_iommu_unmap(tegra, bo);

	if (bo->pages && bo->vaddr)
		vunmap(bo->vaddr);

	if (gem->import_attach) {
		dma_buf_unmap_attachment(gem->import_attach, bo->sgt,
					 DMA_TO_DEVICE);
		drm_prime_gem_destroy(gem, NULL);
	} else {
		tegra_bo_free(gem->dev, bo);
	}

	drm_gem_object_release(gem);
	kfree(bo);
}

int tegra_bo_dumb_create(struct drm_file *file, struct drm_device *drm,
			 struct drm_mode_create_dumb *args)
{
	unsigned int min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_bo *bo;

	args->pitch = round_up(min_pitch, tegra->pitch_align);
	args->size = args->pitch * args->height;

	bo = tegra_bo_create_with_handle(file, drm, args->size, 0,
					 &args->handle);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	return 0;
}

static vm_fault_t tegra_bo_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *gem = vma->vm_private_data;
	struct tegra_bo *bo = to_tegra_bo(gem);
	struct page *page;
	pgoff_t offset;

	if (!bo->pages)
		return VM_FAULT_SIGBUS;

	offset = (vmf->address - vma->vm_start) >> PAGE_SHIFT;
	page = bo->pages[offset];

	return vmf_insert_page(vma, vmf->address, page);
}

const struct vm_operations_struct tegra_bo_vm_ops = {
	.fault = tegra_bo_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

int __tegra_gem_mmap(struct drm_gem_object *gem, struct vm_area_struct *vma)
{
	struct drm_device *drm = gem->dev;
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct tegra_bo *bo = to_tegra_bo(gem);
	unsigned long vm_pgoff = vma->vm_pgoff;
	int err;

	if (bo->flags & TEGRA_BO_HOST1X_GATHER) {
		/*
		 * Clear the VM_PFNMAP flag that was set by drm_gem_mmap(),
		 * and set the vm_pgoff (used as a fake buffer offset by DRM)
		 * to 0 as we want to map the whole buffer.
		 */
		vma->vm_flags &= ~VM_PFNMAP;
		vma->vm_pgoff = 0;

		err = host1x_bo_mmap(host, bo->host1x_bo, vma);
		if (err) {
			drm_gem_vm_close(vma);
			return err;
		}

		vma->vm_pgoff = vm_pgoff;
	} else if (!bo->pages) {
		/*
		 * Clear the VM_PFNMAP flag that was set by drm_gem_mmap(),
		 * and set the vm_pgoff (used as a fake buffer offset by DRM)
		 * to 0 as we want to map the whole buffer.
		 */
		vma->vm_flags &= ~VM_PFNMAP;
		vma->vm_pgoff = 0;

		err = dma_mmap_attrs(gem->dev->dev, vma, bo->dma_cookie,
				     bo->paddr, gem->size, bo->dma_attrs);
		if (err < 0) {
			drm_gem_vm_close(vma);
			return err;
		}

		vma->vm_pgoff = vm_pgoff;
	} else {
		pgprot_t prot = vm_get_page_prot(vma->vm_flags);

		vma->vm_flags |= VM_MIXEDMAP;
		vma->vm_flags &= ~VM_PFNMAP;

		vma->vm_page_prot = pgprot_writecombine(prot);
	}

	return 0;
}

int tegra_drm_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct drm_gem_object *gem;
	int err;

	err = drm_gem_mmap(file, vma);
	if (err < 0)
		return err;

	gem = vma->vm_private_data;

	return __tegra_gem_mmap(gem, vma);
}

static struct sg_table *
tegra_gem_prime_map_dma_buf(struct dma_buf_attachment *attach,
			    enum dma_data_direction dir)
{
	struct drm_gem_object *gem = attach->dmabuf->priv;
	struct tegra_bo *bo = to_tegra_bo(gem);
	struct sg_table *sgt;

	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return NULL;

	if (bo->pages) {
		if (sg_alloc_table_from_pages(sgt, bo->pages, bo->num_pages,
					      0, gem->size, GFP_KERNEL) < 0)
			goto free_sgt;

		if (dma_map_sgtable(attach->dev, sgt, dir, 0))
			goto free;
	} else {
		if (sg_alloc_table(sgt, 1, GFP_KERNEL))
			goto free_sgt;

		sg_dma_address(sgt->sgl) = bo->paddr;
		sg_dma_len(sgt->sgl) = gem->size;
	}

	return sgt;

free:
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
	return NULL;
}

static void tegra_gem_prime_unmap_dma_buf(struct dma_buf_attachment *attach,
					  struct sg_table *sgt,
					  enum dma_data_direction dir)
{
	struct drm_gem_object *gem = attach->dmabuf->priv;
	struct tegra_bo *bo = to_tegra_bo(gem);

	if (bo->pages)
		dma_unmap_sgtable(attach->dev, sgt, dir, 0);

	sg_free_table(sgt);
	kfree(sgt);
}

static void tegra_gem_prime_release(struct dma_buf *buf)
{
	drm_gem_dmabuf_release(buf);
}

static int tegra_gem_prime_begin_cpu_access(struct dma_buf *buf,
					    enum dma_data_direction direction)
{
	struct drm_gem_object *gem = buf->priv;
	struct tegra_bo *bo = to_tegra_bo(gem);
	struct drm_device *drm = gem->dev;

	if (bo->sgt)
		dma_sync_sgtable_for_cpu(drm->dev, bo->sgt, DMA_FROM_DEVICE);

	return 0;
}

static int tegra_gem_prime_end_cpu_access(struct dma_buf *buf,
					  enum dma_data_direction direction)
{
	struct drm_gem_object *gem = buf->priv;
	struct tegra_bo *bo = to_tegra_bo(gem);
	struct drm_device *drm = gem->dev;

	if (bo->sgt)
		dma_sync_sgtable_for_device(drm->dev, bo->sgt, DMA_TO_DEVICE);

	return 0;
}

static int tegra_gem_prime_mmap(struct dma_buf *buf, struct vm_area_struct *vma)
{
	struct drm_gem_object *gem = buf->priv;
	int err;

	err = drm_gem_mmap_obj(gem, gem->size, vma);
	if (err < 0)
		return err;

	return __tegra_gem_mmap(gem, vma);
}

void *tegra_bo_vmap(struct tegra_bo *bo)
{
	struct drm_device *drm = bo->gem.dev;
	struct tegra_drm *tegra = drm->dev_private;

	mutex_lock(&tegra->mm_lock);
	if (!bo->vaddr && bo->pages) {
		bo->vaddr = vmap(bo->pages, bo->num_pages, VM_MAP,
				 pgprot_writecombine(PAGE_KERNEL));
	}
	mutex_unlock(&tegra->mm_lock);

	return bo->vaddr;
}

static int tegra_gem_prime_vmap(struct dma_buf *buf, struct dma_buf_map *map)
{
	struct drm_gem_object *gem = buf->priv;
	struct tegra_bo *bo = to_tegra_bo(gem);

	if (gem->import_attach)
		return dma_buf_vmap(gem->import_attach->dmabuf, map);

	dma_buf_map_set_vaddr(map, tegra_bo_vmap(bo));

	return 0;
}

static void tegra_gem_prime_vunmap(struct dma_buf *buf, struct dma_buf_map *map)
{
	struct drm_gem_object *gem = buf->priv;

	if (gem->import_attach)
		dma_buf_vunmap(gem->import_attach->dmabuf, map);
}

static const struct dma_buf_ops tegra_gem_prime_dmabuf_ops = {
	.map_dma_buf = tegra_gem_prime_map_dma_buf,
	.unmap_dma_buf = tegra_gem_prime_unmap_dma_buf,
	.release = tegra_gem_prime_release,
	.begin_cpu_access = tegra_gem_prime_begin_cpu_access,
	.end_cpu_access = tegra_gem_prime_end_cpu_access,
	.mmap = tegra_gem_prime_mmap,
	.vmap = tegra_gem_prime_vmap,
	.vunmap = tegra_gem_prime_vunmap,
};

struct dma_buf *tegra_gem_prime_export(struct drm_gem_object *gem,
				       int flags)
{
	struct tegra_bo *bo = to_tegra_bo(gem);
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	if (bo->flags & TEGRA_BO_HOST1X_GATHER)
		return ERR_PTR(-EINVAL);

	exp_info.exp_name = KBUILD_MODNAME;
	exp_info.owner = gem->dev->driver->fops->owner;
	exp_info.ops = &tegra_gem_prime_dmabuf_ops;
	exp_info.size = gem->size;
	exp_info.flags = flags;
	exp_info.priv = gem;

	return drm_gem_dmabuf_export(gem->dev, &exp_info);
}

struct drm_gem_object *tegra_gem_prime_import(struct drm_device *drm,
					      struct dma_buf *buf)
{
	struct tegra_bo *bo;

	if (buf->ops == &tegra_gem_prime_dmabuf_ops) {
		struct drm_gem_object *gem = buf->priv;

		if (gem->dev == drm) {
			drm_gem_object_get(gem);
			return gem;
		}
	}

	bo = tegra_bo_import(drm, buf);
	if (IS_ERR(bo))
		return ERR_CAST(bo);

	return &bo->gem;
}
