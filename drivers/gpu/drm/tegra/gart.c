/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/module.h>

#include "drm.h"
#include "gart.h"
#include "job.h"

#define GART_SECURITY_GAP	SZ_4K

/*
 * By default only scattered BOs are mapped into GART, the security parameter
 * allow to change this behaviour in a trade-off to performance and resources
 * availability (complex HW jobs will fail due to IOVA space shortage in a
 * strict mode).
 */
static unsigned int gart_security_level = 0;

/*
 * Security levels starting with the least secure level 0:
 *
 * 0 = scattered BOs are mapped
 * 1 = level 0 + scattered BOs are unmapped (into cache)
 * 2 = level 1 + writable  contiguous BOs are mapped whenever possible
 * 3 = level 2 + read-only contiguous BOs are mapped whenever possible
 * 4 = all BOs are mapped and unmapped (into cache)
 * 5 = level 4 + 4K canary gap between mappings for trapping OOB accesses
 * 6 = level 5 + caching disabled
 */
module_param(gart_security_level, uint, 0644);
MODULE_PARM_DESC(gart_security_level, "Memory protection level (0..6)");

static bool
tegra_bo_mm_evict_bo(struct tegra_drm *tegra, struct tegra_bo *bo,
		     bool release, bool unmap, bool sanitize)
{
	size_t bytes;

	DRM_DEBUG("%p: evict %d release %d unmap %d sanitize %d iosize %zu\n",
		  bo, !list_empty(&bo->mm_eviction_entry), release, unmap,
		  sanitize, bo->gem.size);

	if (list_empty(&bo->mm_eviction_entry))
		return false;

	if (unmap && gart_security_level > 0) {
		bytes = iommu_unmap(tegra->domain, bo->gartaddr, bo->gem.size);
		if (bytes != bo->gem.size)
			DRM_ERROR("failed to unmap bo\n");
	}

	if (release)
		drm_mm_remove_node(&bo->mm);

	if (sanitize)
		bo->gartaddr = TEGRA_POISON_ADDR;

	list_del_init(&bo->mm_eviction_entry);

	return true;
}

static void
tegra_bo_mm_release_victims(struct tegra_drm *tegra,
			    struct list_head *victims_list,
			    bool cleanup,
			    dma_addr_t start,
			    dma_addr_t end)
{
	dma_addr_t victim_start;
	dma_addr_t victim_end;
	struct tegra_bo *tmp;
	struct tegra_bo *bo;
	size_t bytes;
	size_t size;

	/*
	 * Remove BO from MM and unmap only part of BO that is outside of
	 * the given [star, end) range. The overlapping region will be mapped
	 * by a new BO shortly, this reduces re-mapping overhead.
	 */
	list_for_each_entry_safe(bo, tmp, victims_list, mm_eviction_entry) {
		if (!cleanup && gart_security_level > 0) {
			victim_end = bo->gartaddr + bo->gem.size;
			victim_start = bo->gartaddr;

			if (victim_start < start) {
				size = start - victim_start;

				bytes = iommu_unmap(tegra->domain,
						    victim_start, size);
				if (bytes != size)
					DRM_ERROR("failed to unmap bo\n");
			}

			if (victim_end > end) {
				size = victim_end - end;

				bytes = iommu_unmap(tegra->domain, end, size);
				if (bytes != size)
					DRM_ERROR("failed to unmap bo\n");
			}
		}

		tegra_bo_mm_evict_bo(tegra, bo, false, cleanup, true);
	}
}

static bool
tegra_bo_mm_evict_something(struct tegra_drm *tegra,
			    struct list_head *victims_list,
			    size_t size)
{
	LIST_HEAD(scan_list);
	struct list_head *eviction_list;
	struct drm_mm_scan scan;
	struct tegra_bo *tmp;
	struct tegra_bo *bo;
	unsigned long order;
	bool found = false;

	eviction_list = &tegra->mm_eviction_list;
	order = __ffs(tegra->domain->pgsize_bitmap);

	if (list_empty(eviction_list))
		return false;

	drm_mm_scan_init(&scan, &tegra->mm, size, 1UL << order, 0,
			 DRM_MM_INSERT_BEST);

	list_for_each_entry_safe(bo, tmp, eviction_list, mm_eviction_entry) {
		/* move BO from eviction to scan list */
		list_move(&bo->mm_eviction_entry, &scan_list);

		/* check whether hole has been found */
		if (drm_mm_scan_add_block(&scan, &bo->mm)) {
			found = true;
			break;
		}
	}

	list_for_each_entry_safe(bo, tmp, &scan_list, mm_eviction_entry) {
		/*
		 * We can't release BO's mm node here, see comments to
		 * drm_mm_scan_remove_block() in drm_mm.c
		 */
		if (drm_mm_scan_remove_block(&scan, &bo->mm))
			list_move(&bo->mm_eviction_entry, victims_list);
		else
			list_move(&bo->mm_eviction_entry, eviction_list);
	}

	/*
	 * Victims would be unmapped later, only mark them as released
	 * for now.
	 */
	list_for_each_entry(bo, victims_list, mm_eviction_entry) {
		DRM_DEBUG("%p\n", bo);
		drm_mm_remove_node(&bo->mm);
	}

	return found;
}

/*
 * GART's aperture has a limited size of 32MB and we want to avoid frequent
 * remappings. To reduce the number of remappings, the mappings are not
 * getting released (say stay in cache) until there is no space in the GART
 * or BO is destroyed. Once there is not enough space for the mapping, the
 * DRM's MM scans mappings for a suitable hole and tells what cached mappings
 * should be released in order to free up enough space for the mapping to
 * succeed.
 */
static int tegra_bo_gart_map_locked(struct tegra_drm *tegra,
				    struct tegra_bo *bo,
				    bool enospc_fatal)
{
	unsigned long order = __ffs(tegra->domain->pgsize_bitmap);
	enum drm_mm_insert_mode insert_mode;
	LIST_HEAD(victims_list);
	size_t gart_size;
	size_t map_size;
	size_t iosize;
	int err;

	DRM_DEBUG("%p: iomap_cnt %u\n", bo, bo->iomap_cnt);

	/* check whether BO is already mapped */
	if (bo->iomap_cnt++)
		return 0;

	/* if BO is on the eviction list, just remove it from the list */
	if (tegra_bo_mm_evict_bo(tegra, bo, false, false, false))
		return 0;

	/* BO shall not be mapped from other places */
	WARN_ON_ONCE(drm_mm_node_allocated(&bo->mm));

	map_size = bo->gem.size;

	if (gart_security_level > 4)
		map_size += GART_SECURITY_GAP;

	/*
	 * Optimize allocation strategy by pinning smaller BOs at the
	 * top of the GART.
	 */
	if (map_size < SZ_512K)
		insert_mode = DRM_MM_INSERT_HIGH;
	else
		insert_mode = DRM_MM_INSERT_LOW;

	err = drm_mm_insert_node_generic(&tegra->mm, &bo->mm, map_size,
					 1UL << order, 0, insert_mode);
	if (!err)
		goto mm_ok;

	/*
	 * If there is not enough room in GART, release cached mappings
	 * and try again. Otherwise error out.
	 */
	if (err != -ENOSPC)
		goto mm_err;

	gart_size = tegra->domain->geometry.aperture_end + 1 -
		    tegra->domain->geometry.aperture_start;

	/* check whether BO could be squeezed into GART at all */
	if (map_size > gart_size) {
		err = enospc_fatal ? -ENOMEM : -ENOSPC;
		goto mm_err;
	}

	/*
	 * Scan for a suitable hole conjointly with a cached mappings
	 * and release mappings from cache if needed.
	 */
	if (!tegra_bo_mm_evict_something(tegra, &victims_list, map_size))
		goto mm_err;

	/*
	 * We have freed some of the cached mappings and now reservation
	 * should succeed.
	 */
	err = drm_mm_insert_node_generic(&tegra->mm, &bo->mm, map_size,
					 1UL << order, 0, DRM_MM_INSERT_EVICT);
	if (err)
		goto mm_err;

mm_ok:
	bo->gartaddr = bo->mm.start;

	iosize = iommu_map_sgtable(tegra->domain, bo->gartaddr, bo->sgt,
				   IOMMU_READ | IOMMU_WRITE);
	if (iosize != bo->gem.size) {
		DRM_ERROR("mapping failed %zu %zu\n", iosize, bo->gem.size);
		drm_mm_remove_node(&bo->mm);
		err = -ENOMEM;
	}

mm_err:
	if (err) {
		if (err != -ENOSPC || drm_debug_enabled(DRM_UT_DRIVER))
			DRM_ERROR("failed size %zu: %d\n", map_size, err);

		bo->gartaddr = TEGRA_POISON_ADDR;
		bo->iomap_cnt = 0;

		/* nuke all affected victims */
		tegra_bo_mm_release_victims(tegra, &victims_list, true, 0, 0);
	} else {
		/*
		 * Unmap all affected victims, excluding the newly mapped
		 * BO range.
		 */
		tegra_bo_mm_release_victims(tegra, &victims_list, false,
					    bo->gartaddr,
					    bo->gartaddr + bo->gem.size);

		DRM_DEBUG("%p success iosize %zu gartaddr %08x\n",
			  bo, bo->gem.size, bo->gartaddr);
	}

	return err;
}

void tegra_bo_gart_unmap_locked(struct tegra_drm *tegra, struct tegra_bo *bo)
{
	bool on_eviction_list = !list_empty(&bo->mm_eviction_entry);
	struct drm_device *drm = tegra->drm;

	if (drm_WARN_ONCE(drm, !on_eviction_list && !bo->iomap_cnt,
			  "imbalanced bo %p unmapping %u\n", bo, bo->iomap_cnt))
		return;

	/* put mapping into the eviction cache */
	if (!on_eviction_list)
		list_add(&bo->mm_eviction_entry, &tegra->mm_eviction_list);

	tegra_bo_mm_evict_bo(tegra, bo, true, true, true);
}

static void
tegra_bo_gart_unmap_cached_locked(struct tegra_drm *tegra, struct tegra_bo *bo,
				  bool flush_cache)
{
	struct drm_device *drm = tegra->drm;

	if (drm_WARN_ONCE(drm, !bo->iomap_cnt,
			  "imbalanced bo %p unmapping\n", bo))
		return;

	DRM_DEBUG("%p iomap_cnt %u\n", bo, bo->iomap_cnt);

	/* put mapping into the eviction cache */
	if (--bo->iomap_cnt == 0) {
		list_add(&bo->mm_eviction_entry, &tegra->mm_eviction_list);

		/* and release it entirely if necessary */
		if (flush_cache)
			tegra_bo_mm_evict_bo(tegra, bo, true, true, true);
	}
}

void tegra_drm_gart_flush_cache_locked(struct tegra_drm *tegra)
{
	struct drm_mm_node *mm, *tmp;
	struct tegra_bo *bo;

	drm_mm_for_each_node_safe(mm, tmp, &tegra->mm) {
		bo = container_of(mm, struct tegra_bo, mm);

		DRM_DEBUG("%p gem_size %zu gartaddr %08x iomap_cnt %u\n",
			  bo, bo->gem.size, bo->gartaddr, bo->iomap_cnt);

		tegra_bo_mm_evict_bo(tegra, bo, true, true, true);
	}
}

void tegra_drm_job_unmap_gart_locked(struct tegra_drm *tegra,
				     struct tegra_bo **bos,
				     unsigned int num_bos,
				     unsigned long *bos_gart_bitmap,
				     bool flush_cache)
{
	unsigned int i;

	if (gart_security_level > 5)
		flush_cache = true;

	DRM_DEBUG("flush_cache %d\n", flush_cache);

	for_each_set_bit(i, bos_gart_bitmap, num_bos)
		tegra_bo_gart_unmap_cached_locked(tegra, bos[i], flush_cache);

	bitmap_clear(bos_gart_bitmap, 0, num_bos);

	if (flush_cache)
		tegra_drm_gart_flush_cache_locked(tegra);
}

static int
tegra_drm_job_pre_check_gart_space(struct tegra_drm *tegra,
				   struct tegra_bo **bos,
				   unsigned int num_bos,
				   unsigned long *bos_gart_bitmap,
				   bool *gart_busy,
				   unsigned int security)
{
	struct drm_mm_node *mm;
	struct tegra_bo *bo;
	size_t sparse_size = 0;
	size_t unmapped_size = 0;
	size_t gart_free_size;
	size_t gart_size;
	unsigned int i;
	bool mapped;

	for (i = 0; i < num_bos; i++) {
		bo = bos[i];

		/* all job's BOs must be unmapped now */
		mapped = test_bit(i, bos_gart_bitmap);
		if (WARN_ON_ONCE(mapped))
			return -EINVAL;

		/* gathers are a property of host1x */
		if (bo->flags & TEGRA_BO_HOST1X_GATHER)
			continue;

		if (security > 3 || bo->sgt->nents > 1)
			sparse_size += bo->gem.size;

		if (bo->sgt->nents > 1 && !drm_mm_node_allocated(&bo->mm)) {
			unmapped_size += bo->gem.size;

			DRM_DEBUG("%p gem_size %zu\n", bo, bo->gem.size);
		}
	}

	/* no sparse BOs? good, we're done */
	if (!sparse_size)
		return 0;

	gart_size = tegra->domain->geometry.aperture_end + 1 -
		    tegra->domain->geometry.aperture_start;

	/*
	 * If total size of sparse allocations is larger than the
	 * GART's aperture, then there is nothing we could do about it.
	 *
	 * Userspace need to take that into account.
	 */
	if (sparse_size > gart_size)
		return -ENOMEM;

	gart_free_size = gart_size;

	/*
	 * Get idea about the free space by not taking into account memory
	 * fragmentation.
	 */
	drm_mm_for_each_node(mm, &tegra->mm) {
		bo = container_of(mm, struct tegra_bo, mm);

		if (list_empty(&bo->mm_eviction_entry)) {
			gart_free_size -= bo->gem.size;

			/*
			 * Right now GART is used by other job if pinned BO
			 * doesn't belong to this job.
			 */
			for (i = 0; !(*gart_busy) && i < num_bos; i++) {
				if (bo == bos[i])
					break;
			}

			*gart_busy |= (i == num_bos);

			DRM_DEBUG("%p pinned gem_size %zu gartaddr %08x iomap_cnt %u\n",
				  bo, bo->gem.size, bo->gartaddr, bo->iomap_cnt);
		} else {
			DRM_DEBUG("%p cached gem_size %zu gartaddr %08x\n",
				  bo, bo->gem.size, bo->gartaddr);
		}
	}

	/*
	 * No way allocation could succeed if the GART's free area is
	 * smaller than the needed amount.
	 */
	if (unmapped_size > gart_free_size)
		return -ENOSPC;

	DRM_DEBUG("gart_busy %d\n", *gart_busy);

	return 0;
}

static bool
tegra_drm_job_is_largest_unmapped_bo(unsigned int k,
				     struct tegra_bo **bos,
				     unsigned int num_bos,
				     unsigned long *bos_gart_bitmap)
{
	unsigned int i;

	for (i = 0; i < num_bos; i++) {
		if (i == k)
			continue;

		if (bos[i]->flags & TEGRA_BO_HOST1X_GATHER)
			continue;

		if (test_bit(i, bos_gart_bitmap))
			continue;

		if (bos[i]->sgt->nents == 1)
			continue;

		if (bos[i]->gem.size > bos[k]->gem.size)
			return false;
	}

	return true;
}

/*
 * Map job BOs into the GART aperture. Due to limited size of the aperture,
 * mapping of contiguous allocations is optional and we're trying to map
 * everything till no aperture space left. Mapping of scattered allocations
 * is mandatory because there is no other way to handle these allocations.
 * If there is not enough space in GART, then all succeeded mappings are
 * unmapped and caller should try again after "gart_free_up" completion is
 * signalled. Note that GART doesn't make system secure and only improves
 * system stability by providing some optional protection for memory from a
 * badly-behaving hardware.
 */
int tegra_drm_job_map_gart_locked(struct tegra_drm *tegra,
				  struct tegra_bo **bos,
				  unsigned int num_bos,
				  unsigned long *bos_write_bitmap,
				  unsigned long *bos_gart_bitmap)
{
	struct tegra_bo *bo;
	bool gart_busy = false;
	bool retried = false;
	bool again = false;
	bool largest;
	unsigned int security;
	unsigned int i;
	int err;

	security = gart_security_level;

	/* quickly check whether job could be handled by GART at all */
	err = tegra_drm_job_pre_check_gart_space(tegra, bos, num_bos,
						 bos_gart_bitmap,
						 &gart_busy,
						 security);
	if (err) {
		if (err == -ENOSPC)
			goto err_retry;

		return err;
	}

	/* map all scattered BOs, this must not fail */
map_scattered:
	for (i = 0; i < num_bos; i++) {
		bo = bos[i];

		/* gathers are a property of host1x */
		if (bo->flags & TEGRA_BO_HOST1X_GATHER)
			continue;

		/* go next if already mapped */
		if (test_bit(i, bos_gart_bitmap))
			continue;

		/* go next if contiguous */
		if (bo->sgt->nents == 1)
			continue;

		/*
		 * In order to optimize mapping layout, the largest BOs are
		 * mapped first.
		 */
		largest = tegra_drm_job_is_largest_unmapped_bo(i, bos, num_bos,
							       bos_gart_bitmap);
		if (!largest) {
			again = true;
			continue;
		}

		err = tegra_bo_gart_map_locked(tegra, bo, true);
		if (err)
			goto err_unmap;

		set_bit(i, bos_gart_bitmap);
	}

	if (again) {
		again = false;
		goto map_scattered;
	}

	if (!security)
		return 0;

	/* then map the writable BOs */
	for_each_set_bit(i, bos_write_bitmap, num_bos) {
		bo = bos[i];

		/* go next if already mapped */
		if (test_bit(i, bos_gart_bitmap))
			continue;

		/* gathers are a property of host1x */
		if (bo->flags & TEGRA_BO_HOST1X_GATHER)
			continue;

		/* go next if GART has no space */
		err = tegra_bo_gart_map_locked(tegra, bo, false);

		if (err == -ENOSPC && security < 4)
			continue;

		if (err)
			goto err_unmap;

		set_bit(i, bos_gart_bitmap);
	}

	if (security < 3)
		return 0;

	/* then map the read-only BOs */
	for (i = 0; i < num_bos; i++) {
		bo = bos[i];

		/* go next if already mapped */
		if (test_bit(i, bos_gart_bitmap))
			continue;

		/* gathers are a property of host1x */
		if (bo->flags & TEGRA_BO_HOST1X_GATHER)
			continue;

		/* go next if GART has no space */
		err = tegra_bo_gart_map_locked(tegra, bo, false);

		if (err == -ENOSPC && security < 4)
			continue;

		if (err)
			goto err_unmap;

		set_bit(i, bos_gart_bitmap);
	}

	return 0;

err_unmap:
	/*
	 * The entire cache needs to be wiped on ENOSPC because this means
	 * that there is enough space in the cache, but allocator selected
	 * inappropriate allocation strategy which results in the failure.
	 * The cleared cache will help allocator to succeed.
	 *
	 * If GART is busy (used by other job), then there is no need to
	 * flush entire cache, but instead just try again next time, once
	 * the other job will be released.
	 */
	tegra_drm_job_unmap_gart_locked(tegra, bos, num_bos, bos_gart_bitmap,
					!gart_busy && err == -ENOSPC);

	if (!gart_busy && err == -ENOSPC && !retried) {
		retried = true;
		goto map_scattered;
	}

	/*
	 * Caller should retry if GART has no space, but allocation could
	 * succeed after freeing some space.
	 */
	if (err == -ENOSPC && !retried) {
err_retry:
		reinit_completion(&tegra->gart_free_up);
		return -EAGAIN;
	}

	return err;
}

int tegra_drm_gart_map_optional(struct tegra_drm *tegra, struct tegra_bo *bo)
{
	int err;

	if (!IS_ENABLED(CONFIG_TEGRA_IOMMU_GART) || !tegra->has_gart)
		return 0;

	/*
	 * Mapping of contiguous BOs isn't strictly necessary, hence that's
	 * why there is 'optional' postfix in the function's name.
	 */
	if (bo->sgt->nents == 1)
		return 0;

	mutex_lock(&tegra->mm_lock);
	err = tegra_bo_gart_map_locked(tegra, bo, true);
	mutex_unlock(&tegra->mm_lock);

	return err ?: 1;
}

void tegra_drm_gart_unmap_optional(struct tegra_drm *tegra, struct tegra_bo *bo)
{
	if (!IS_ENABLED(CONFIG_TEGRA_IOMMU_GART) || !tegra->has_gart)
		return;

	if (bo->sgt->nents == 1)
		return;

	mutex_lock(&tegra->mm_lock);
	tegra_bo_gart_unmap_cached_locked(tegra, bo, false);
	mutex_unlock(&tegra->mm_lock);
}
