// SPDX-License-Identifier: GPL-2.0
/*
 * Resource manager for Octeon.
 *
 * Copyright (C) 2017 Cavium, Inc.
 */
#include <linux/module.h>

#include <asm/octeon/octeon.h>
#include <asm/octeon/cvmx-bootmem.h>

#define RESOURCE_MGR_BLOCK_NAME		"cvmx-global-resources"
#define MAX_RESOURCES			128
#define INST_AVAILABLE			-88
#define OWNER				0xbadc0de

struct global_resource_entry {
	struct global_resource_tag tag;
	u64 phys_addr;
	u64 size;
};

struct global_resources {
#ifdef __LITTLE_ENDIAN_BITFIELD
	u32 rlock;
	u32 pad;
#else
	u32 pad;
	u32 rlock;
#endif
	u64 entry_cnt;
	struct global_resource_entry resource_entry[];
};

static struct global_resources *res_mgr_info;


/*
 * The resource manager interacts with software running outside of the
 * Linux kernel, which necessitates locking to maintain data structure
 * consistency.  These custom locking functions implement the locking
 * protocol, and cannot be replaced by kernel locking functions that
 * may use different in-memory structures.
 */

static void res_mgr_lock(void)
{
	while (cmpxchg(&res_mgr_info->rlock, 0, 1))
		; /* Loop while not zero */
	rmb();
}

static void res_mgr_unlock(void)
{
	/* Wait until all resource operations finish before unlocking. */
	wmb();
	WRITE_ONCE(res_mgr_info->rlock, 0);
	/* Force a write buffer flush. */
	wmb();
}

static int res_mgr_find_resource(struct global_resource_tag tag)
{
	struct global_resource_entry *res_entry;
	int i;

	for (i = 0; i < res_mgr_info->entry_cnt; i++) {
		res_entry = &res_mgr_info->resource_entry[i];
		if (res_entry->tag.lo == tag.lo && res_entry->tag.hi == tag.hi)
			return i;
	}
	return -1;
}

/**
 * res_mgr_create_resource() - Create a resource.
 * @tag: Identifies the resource.
 * @inst_cnt: Number of resource instances to create.
 *
 * Returns 0 if the source was created successfully.
 * Returns < 0 for error codes.
 */
int res_mgr_create_resource(struct global_resource_tag tag, int inst_cnt)
{
	struct global_resource_entry *res_entry;
	u64 size;
	u64 *res_addr;
	int res_index, i, rc = 0;

	res_mgr_lock();

	/* Make sure resource doesn't already exist. */
	res_index = res_mgr_find_resource(tag);
	if (res_index >= 0) {
		rc = -EEXIST;
		goto err;
	}

	if (res_mgr_info->entry_cnt >= MAX_RESOURCES) {
		pr_err("Resource max limit reached, not created\n");
		rc = -ENOSPC;
		goto err;
	}

	/*
	 * Each instance is kept in an array of u64s. The first array element
	 * holds the number of allocated instances.
	 */
	size = sizeof(u64) * (inst_cnt + 1);
	res_addr = cvmx_bootmem_alloc_range(size, CVMX_CACHE_LINE_SIZE, 0, 0);
	if (!res_addr) {
		pr_err("Failed to allocate resource. not created\n");
		rc = -ENOMEM;
		goto err;
	}

	/* Initialize the newly created resource. */
	*res_addr = inst_cnt;
	for (i = 1; i <= inst_cnt; i++)
		res_addr[i] = INST_AVAILABLE;

	res_index = res_mgr_info->entry_cnt;
	res_entry = &res_mgr_info->resource_entry[res_index];
	res_entry->tag = tag;
	res_entry->phys_addr = virt_to_phys(res_addr);
	res_entry->size = size;
	res_mgr_info->entry_cnt++;

err:
	res_mgr_unlock();

	return rc;
}
EXPORT_SYMBOL(res_mgr_create_resource);

/**
 * res_mgr_alloc_range() - Allocate a range of resource instances.
 * @tag: Identifies the resource.
 * @req_inst: Requested start of instance range to allocate.
 *	      Range instances are guaranteed to be sequential
 *	      (-1 for don't care).
 * @req_cnt: Number of instances to allocate.
 * @use_last_avail: Set to request the last available instance.
 * @inst: Updated with the allocated instances.
 *
 * Returns 0 if the source was created successfully.
 * Returns < 0 for error codes.
 */
int res_mgr_alloc_range(struct global_resource_tag tag, int req_inst,
			int req_cnt, bool use_last_avail, int *inst)
{
	struct global_resource_entry *res_entry;
	int res_index;
	u64 *res_addr;
	u64 inst_cnt;
	int alloc_cnt, i, rc = -ENOENT;

	/* Start with no instances allocated. */
	for (i = 0; i < req_cnt; i++)
		inst[i] = INST_AVAILABLE;

	res_mgr_lock();

	/* Find the resource. */
	res_index = res_mgr_find_resource(tag);
	if (res_index < 0) {
		pr_err("Resource not found, can't allocate instance\n");
		goto err;
	}

	/* Get resource data. */
	res_entry = &res_mgr_info->resource_entry[res_index];
	res_addr = phys_to_virt(res_entry->phys_addr);
	inst_cnt = *res_addr;

	/* Allocate the requested instances. */
	if (req_inst >= 0) {
		/* Specific instance range requested. */
		if (req_inst + req_cnt >= inst_cnt) {
			pr_err("Requested instance out of range\n");
			goto err;
		}

		for (i = 0; i < req_cnt; i++) {
			if (*(res_addr + req_inst + 1 + i) == INST_AVAILABLE) {
				inst[i] = req_inst + i;
			} else {
				inst[0] = INST_AVAILABLE;
				break;
			}
		}
	} else if (use_last_avail) {
		/* Last available instance requested. */
		alloc_cnt = 0;
		for (i = inst_cnt; i > 0; i--) {
			if (*(res_addr + i) == INST_AVAILABLE) {
				/*
				 * Instance off by 1 (first element holds the
				 * count).
				 */
				inst[alloc_cnt] = i - 1;

				alloc_cnt++;
				if (alloc_cnt == req_cnt)
					break;
			}
		}

		if (i == 0)
			inst[0] = INST_AVAILABLE;
	} else {
		/* Next available instance requested. */
		alloc_cnt = 0;
		for (i = 1; i <= inst_cnt; i++) {
			if (*(res_addr + i) == INST_AVAILABLE) {
				/*
				 * Instance off by 1 (first element holds the
				 * count).
				 */
				inst[alloc_cnt] = i - 1;

				alloc_cnt++;
				if (alloc_cnt == req_cnt)
					break;
			}
		}

		if (i > inst_cnt)
			inst[0] = INST_AVAILABLE;
	}

	if (inst[0] != INST_AVAILABLE) {
		for (i = 0; i < req_cnt; i++)
			*(res_addr + inst[i] + 1) = OWNER;
		rc = 0;
	}

err:
	res_mgr_unlock();

	return rc;
}
EXPORT_SYMBOL(res_mgr_alloc_range);

/**
 * res_mgr_alloc() - Allocate a resource instance.
 * @tag: Identifies the resource.
 * @req_inst: Requested instance to allocate (-1 for don't care).
 * @use_last_avail: Set to request the last available instance.
 *
 * Returns: Allocated resource instance if successful.
 * Returns <0 for error codes.
 */
int res_mgr_alloc(struct global_resource_tag tag, int req_inst,
		  bool use_last_avail)
{
	int inst, rc;

	rc = res_mgr_alloc_range(tag, req_inst, 1, use_last_avail, &inst);
	if (!rc)
		return inst;
	return rc;
}
EXPORT_SYMBOL(res_mgr_alloc);

/**
 * res_mgr_free_range() - Free a resource instance range.
 * @tag: Identifies the resource.
 * @inst: Requested instance to free.
 * @req_cnt: Number of instances to free.
 */
void res_mgr_free_range(struct global_resource_tag tag, const int *inst,
			int req_cnt)
{
	struct global_resource_entry *res_entry;
	int res_index, i;
	u64 *res_addr;

	res_mgr_lock();

	/* Find the resource. */
	res_index = res_mgr_find_resource(tag);
	if (res_index < 0) {
		pr_err("Resource not found, can't free instance\n");
		goto err;
	}

	/* Get the resource data. */
	res_entry = &res_mgr_info->resource_entry[res_index];
	res_addr = phys_to_virt(res_entry->phys_addr);

	/* Free the resource instances. */
	for (i = 0; i < req_cnt; i++) {
		/* Instance off by 1 (first element holds the count). */
		*(res_addr + inst[i] + 1) = INST_AVAILABLE;
	}

err:
	res_mgr_unlock();
}
EXPORT_SYMBOL(res_mgr_free_range);

/**
 * res_mgr_free() - Free a resource instance.
 * @tag: Identifies the resource.
 * @req_inst: Requested instance to free.
 */
void res_mgr_free(struct global_resource_tag tag, int inst)
{
	res_mgr_free_range(tag, &inst, 1);
}
EXPORT_SYMBOL(res_mgr_free);

static int __init res_mgr_init(void)
{
	struct cvmx_bootmem_named_block_desc *block;
	int block_size;
	u64 addr;

	cvmx_bootmem_lock();

	/* Search for the resource manager data in boot memory. */
	block = cvmx_bootmem_phy_named_block_find(RESOURCE_MGR_BLOCK_NAME,
						  CVMX_BOOTMEM_FLAG_NO_LOCKING);
	if (block) {
		/* Found. */
		res_mgr_info = phys_to_virt(block->base_addr);
	} else {
		/* Create it. */
		block_size = sizeof(struct global_resources) +
			sizeof(struct global_resource_entry) * MAX_RESOURCES;
		addr = cvmx_bootmem_phy_named_block_alloc(block_size, 0, 0,
				CVMX_CACHE_LINE_SIZE, RESOURCE_MGR_BLOCK_NAME,
				CVMX_BOOTMEM_FLAG_NO_LOCKING);
		if (!addr) {
			pr_err("Failed to allocate name block %s\n",
			       RESOURCE_MGR_BLOCK_NAME);
		} else {
			res_mgr_info = phys_to_virt(addr);
			memset(res_mgr_info, 0, block_size);
		}
	}

	cvmx_bootmem_unlock();

	return 0;
}
device_initcall(res_mgr_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cavium, Inc. Octeon resource manager");
