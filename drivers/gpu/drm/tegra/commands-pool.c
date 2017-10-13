#include <linux/iommu.h>

#include "commands-pool.h"

struct commands_bucket {
	struct list_head node;
	dma_addr_t phys;
	dma_addr_t dma;
	void *vaddr;
	unsigned long bitmap[];
};

struct tegra_drm_commands_pool {
	struct host1x *host1x;
	struct list_head list;
	struct list_head removal_list;
	struct commands_bucket *available_bucket;
	struct semaphore sem;
	spinlock_t lock;
	size_t block_size;
	unsigned int removal_cnt;
	unsigned int num;
};

static inline struct tegra_drm_commands_bo *to_commands_bo(struct host1x_bo *bo)
{
	return container_of(bo, struct tegra_drm_commands_bo, base);
}

static struct host1x_bo *commands_bo_get(struct host1x_bo *bo)
{
	return bo;
}

static void commands_bo_put(struct host1x_bo *bo)
{
}

static dma_addr_t commands_bo_pin(struct host1x_bo *bo, struct sg_table **sgt)
{
	return to_commands_bo(bo)->dma;
}

static void commands_bo_unpin(struct host1x_bo *bo, struct sg_table *sgt)
{
}

static void *commands_bo_mmap(struct host1x_bo *bo)
{
	return to_commands_bo(bo)->vaddr;
}

static void commands_bo_munmap(struct host1x_bo *bo, void *addr)
{
}

static void *commands_bo_kmap(struct host1x_bo *bo, unsigned int pagenum)
{
	return NULL;
}

static void commands_bo_kunmap(struct host1x_bo *bo, unsigned int pagenum,
			       void *addr)
{
}

static size_t commands_bo_size(struct host1x_bo *bo)
{
	return to_commands_bo(bo)->pool->block_size;
}

static const struct host1x_bo_ops commands_bo_ops = {
	.get = commands_bo_get,
	.put = commands_bo_put,
	.pin = commands_bo_pin,
	.unpin = commands_bo_unpin,
	.mmap = commands_bo_mmap,
	.munmap = commands_bo_munmap,
	.munmap = commands_bo_munmap,
	.kmap = commands_bo_kmap,
	.kunmap = commands_bo_kunmap,
	.size = commands_bo_size,
};

static struct commands_bucket *commands_bucket_alloc(
				struct tegra_drm_commands_pool *pool,
				unsigned long flags)
{
	struct commands_bucket *bucket;

	bucket = kmalloc(sizeof(*bucket) +
			 sizeof(unsigned long) * BITS_TO_LONGS(pool->num),
			 flags);
	if (!bucket)
		return NULL;

	bucket->vaddr = host1x_alloc(pool->host1x,
				     pool->block_size * pool->num,
				     &bucket->dma, &bucket->phys,
				     flags, IOMMU_READ);
	if (!bucket->vaddr)
		goto err_free_pool;

	bitmap_zero(bucket->bitmap, pool->num);
	list_add(&bucket->node, &pool->list);

	return bucket;

err_free_pool:
	kfree(bucket);

	return NULL;
}

static void commands_bucket_destroy(struct tegra_drm_commands_pool *pool,
				    struct commands_bucket *bucket)
{
	list_del(&bucket->node);

	host1x_free(pool->host1x, bucket->vaddr,
		    pool->block_size * pool->num,
		    bucket->dma, bucket->phys);
	kfree(bucket);
}

struct tegra_drm_commands_pool *tegra_drm_commands_pool_create(
					struct drm_device *drm,
					size_t block_size,
					unsigned int entries_num,
					unsigned int buckets_num)
{
	struct tegra_drm_commands_pool *pool;

	/*
	 * Create a dynamically growing / shrinking pool of host1x
	 * allocations, suitable for commands submissions.
	 */
	pool = kmalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return NULL;

	pool->block_size = ALIGN(block_size, 4);
	pool->host1x = dev_get_drvdata(drm->dev->parent);
	pool->num = max(entries_num, 4u);

	sema_init(&pool->sem, max(buckets_num, 1u) * pool->num);
	spin_lock_init(&pool->lock);
	INIT_LIST_HEAD(&pool->removal_list);
	INIT_LIST_HEAD(&pool->list);

	pool->available_bucket = commands_bucket_alloc(pool, GFP_KERNEL);
	if (!pool->available_bucket)
		goto err_nomem;

	return pool;

err_nomem:
	kfree(pool);

	return NULL;
}

void tegra_drm_commands_pool_destroy(struct tegra_drm_commands_pool *pool)
{
	struct commands_bucket *bucket;
	struct commands_bucket *tmp;

	list_for_each_entry_safe(bucket, tmp, &pool->list, node)
		commands_bucket_destroy(pool, bucket);

	list_for_each_entry_safe(bucket, tmp, &pool->removal_list, node)
		commands_bucket_destroy(pool, bucket);

	kfree(pool);
}

struct tegra_drm_commands_bo *tegra_drm_commands_pool_alloc(
					struct tegra_drm_commands_pool *pool)
{
	struct tegra_drm_commands_bo *commands_bo;
	struct commands_bucket *bucket;
	unsigned long offset;
	int err, id;

	/*
	 * In order to limit memory usage, limit the number of the
	 * simultaneous allocations accordingly to the number of bucket
	 * buckets. It's likely that kernel will crash with exhausted
	 * reserved memory pools, let's try to avoid it.
	 */
	err = down_interruptible(&pool->sem);
	if (err)
		return ERR_PTR(err);

	commands_bo = kmalloc(sizeof(*commands_bo), GFP_KERNEL);
	if (!commands_bo) {
		up(&pool->sem);
		return ERR_PTR(-ENOMEM);
	}

	spin_lock(&pool->lock);

	/* at first check if there is a non-full bucket around */
	if (pool->available_bucket) {
		bucket = pool->available_bucket;

		id = find_first_zero_bit(bucket->bitmap, pool->num);
		set_bit(id, bucket->bitmap);

		/*
		 * We'd have to allocate a new bucket on next allocation
		 * if we've got the last slot.
		 */
		if (bitmap_full(bucket->bitmap, pool->num))
			pool->available_bucket = NULL;
	} else {
		/* otherwise search for a bucket that has an empty slot */
		list_for_each_entry(bucket, &pool->list, node) {
			id = find_first_zero_bit(bucket->bitmap, pool->num);

			if (id == pool->num)
				continue;

			goto setbit;
		}

		/* otherwise try to take bucket from removal list */
		bucket = list_first_entry_or_null(&pool->removal_list,
						  struct commands_bucket, node);
		if (bucket) {
			list_move(&bucket->node, &pool->list);

			/*
			 * Reset removal counter so if this bucket would
			 * get exhausted quickly, we'd take another bucket
			 * from the removal list before it'd get destroyed.
			 */
			pool->removal_cnt = 0;
		} else {
			/* otherwise quickly allocate a new bucket */
			bucket = commands_bucket_alloc(pool, GFP_NOWAIT);
			if (!bucket)
				goto unlock;
		}

		/* now that's our empty-and-available bucket */
		pool->available_bucket = bucket;
		id = 0;
	}

setbit:
	set_bit(id, bucket->bitmap);

	pool->removal_cnt++;
unlock:
	spin_unlock(&pool->lock);

	if (bucket) {
		offset = pool->block_size * id;

		commands_bo->dma = bucket->dma + offset;
		commands_bo->vaddr = bucket->vaddr + offset;
		commands_bo->base.ops = &commands_bo_ops;
		commands_bo->pool = pool;
	} else {
		up(&pool->sem);

		kfree(commands_bo);
		commands_bo = ERR_PTR(-ENOMEM);
	}

	return commands_bo;
}

void tegra_drm_commands_pool_free(struct tegra_drm_commands_bo *bo)
{
	struct tegra_drm_commands_pool *pool = bo->pool;
	struct commands_bucket *bucket;
	struct commands_bucket *tmp;
	LIST_HEAD(destroy_list);
	unsigned int removal_threshold;
	bool last_allocation = false;
	size_t pool_size;
	int id;

	spin_lock(&pool->lock);

	/*
	 * Use 1.25 of number of allocations per bucket as the removal
	 * threshold value just because it's an arbitrarily reasonable
	 * value.
	 */
	removal_threshold = pool->num + pool->num / 4;

	/* purge buckets after 'timeout' */
	if (pool->removal_cnt > removal_threshold)
		list_splice_init(&pool->removal_list, &destroy_list);

	pool_size = pool->block_size * pool->num;

	list_for_each_entry(bucket, &pool->list, node) {
		/* find a bucket to which this allocation belongs */
		if (bucket->vaddr > bo->vaddr ||
		    bucket->vaddr + pool_size <= bo->vaddr)
			continue;

		id = (bo->vaddr - bucket->vaddr) / pool->block_size;
		clear_bit(id, bucket->bitmap);

		if (!bitmap_empty(bucket->bitmap, pool->num))
			goto got_bucket;

		/*
		 * This bucket got empty. If it's not the first bucket in
		 * the list, move it to the removal list. Leave the last
		 * bucket alive to avoid allocation burden once DRM would
		 * get active again. The non-first buckets are allocated
		 * from reserved memory pools, so we want to release them.
		 */
		if (!list_is_last(&bucket->node, &pool->list)) {
			list_move(&bucket->node, &pool->removal_list);

			/*
			 * If removal list isn't empty, then resetting
			 * counter would also allow to group buckets
			 * destruction.
			 */
			pool->removal_cnt = 0;

			break;
		}

		last_allocation = true;
got_bucket:
		/*
		 * We have an available bucket now and won't need to
		 * allocate a new one.
		 */
		if (!pool->available_bucket)
			pool->available_bucket = bucket;

		break;
	}

	/*
	 * There is no reason to hold removal list if all buckets are empty.
	 * Empty bucket means that all job submissions have been completed and
	 * no new jobs being submitted. DRM is idling now, hence let's get
	 * rid of the unneeded buckets.
	 */
	if (last_allocation)
		list_splice_init(&pool->removal_list, &destroy_list);

	spin_unlock(&pool->lock);

	/*
	 * After several allocations the removal counter reaches threshold,
	 * now it's time to destroy the idling buckets.
	 */
	list_for_each_entry_safe(bucket, tmp, &destroy_list, node)
		commands_bucket_destroy(pool, bucket);

	up(&pool->sem);

	kfree(bo);
}
