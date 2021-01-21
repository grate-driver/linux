// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright IBM Corporation, 2020
 *
 * Author: Mike Rapoport <rppt@linux.ibm.com>
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/cma.h>
#include <linux/mount.h>
#include <linux/memfd.h>
#include <linux/bitops.h>
#include <linux/printk.h>
#include <linux/pagemap.h>
#include <linux/genalloc.h>
#include <linux/syscalls.h>
#include <linux/memblock.h>
#include <linux/pseudo_fs.h>
#include <linux/secretmem.h>
#include <linux/memcontrol.h>
#include <linux/set_memory.h>
#include <linux/sched/signal.h>

#include <uapi/linux/magic.h>

#include <asm/tlbflush.h>

#include "internal.h"

#undef pr_fmt
#define pr_fmt(fmt) "secretmem: " fmt

/*
 * Define mode and flag masks to allow validation of the system call
 * parameters.
 */
#define SECRETMEM_MODE_MASK	(0x0)
#define SECRETMEM_FLAGS_MASK	SECRETMEM_MODE_MASK

struct secretmem_ctx {
	struct gen_pool *pool;
	unsigned int mode;
};

static struct cma *secretmem_cma;

static int secretmem_account_pages(struct page *page, gfp_t gfp, int order)
{
	int err;

	err = memcg_kmem_charge_page(page, gfp, order);
	if (err)
		return err;

	/*
	 * seceremem caches are unreclaimable kernel allocations, so treat
	 * them as unreclaimable slab memory for VM statistics purposes
	 */
	mod_lruvec_page_state(page, NR_SLAB_UNRECLAIMABLE_B,
			      PAGE_SIZE << order);

	return 0;
}

static void secretmem_unaccount_pages(struct page *page, int order)
{

	mod_lruvec_page_state(page, NR_SLAB_UNRECLAIMABLE_B,
			      -PAGE_SIZE << order);
	memcg_kmem_uncharge_page(page, order);
}

static int secretmem_pool_increase(struct secretmem_ctx *ctx, gfp_t gfp)
{
	unsigned long nr_pages = (1 << PMD_PAGE_ORDER);
	struct gen_pool *pool = ctx->pool;
	unsigned long addr;
	struct page *page;
	int i, err;

	page = cma_alloc(secretmem_cma, nr_pages, PMD_SIZE, gfp & __GFP_NOWARN);
	if (!page)
		return -ENOMEM;

	err = secretmem_account_pages(page, gfp, PMD_PAGE_ORDER);
	if (err)
		goto err_cma_release;

	/*
	 * clear the data left from the prevoius user before dropping the
	 * pages from the direct map
	 */
	for (i = 0; i < nr_pages; i++)
		clear_highpage(page + i);

	err = set_direct_map_invalid_noflush(page, nr_pages);
	if (err)
		goto err_memcg_uncharge;

	addr = (unsigned long)page_address(page);
	err = gen_pool_add(pool, addr, PMD_SIZE, NUMA_NO_NODE);
	if (err)
		goto err_set_direct_map;

	flush_tlb_kernel_range(addr, addr + PMD_SIZE);

	return 0;

err_set_direct_map:
	/*
	 * If a split of PUD-size page was required, it already happened
	 * when we marked the pages invalid which guarantees that this call
	 * won't fail
	 */
	set_direct_map_default_noflush(page, nr_pages);
err_memcg_uncharge:
	secretmem_unaccount_pages(page, PMD_PAGE_ORDER);
err_cma_release:
	cma_release(secretmem_cma, page, nr_pages);
	return err;
}

static void secretmem_free_page(struct secretmem_ctx *ctx, struct page *page)
{
	unsigned long addr = (unsigned long)page_address(page);
	struct gen_pool *pool = ctx->pool;

	gen_pool_free(pool, addr, PAGE_SIZE);
}

static struct page *secretmem_alloc_page(struct secretmem_ctx *ctx,
					 gfp_t gfp)
{
	struct gen_pool *pool = ctx->pool;
	unsigned long addr;
	struct page *page;
	int err;

	if (gen_pool_avail(pool) < PAGE_SIZE) {
		err = secretmem_pool_increase(ctx, gfp);
		if (err)
			return NULL;
	}

	addr = gen_pool_alloc(pool, PAGE_SIZE);
	if (!addr)
		return NULL;

	page = virt_to_page(addr);
	get_page(page);

	return page;
}

static vm_fault_t secretmem_fault(struct vm_fault *vmf)
{
	struct secretmem_ctx *ctx = vmf->vma->vm_file->private_data;
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	pgoff_t offset = vmf->pgoff;
	struct page *page;
	int err;

	if (((loff_t)vmf->pgoff << PAGE_SHIFT) >= i_size_read(inode))
		return vmf_error(-EINVAL);

retry:
	page = find_lock_page(mapping, offset);
	if (!page) {
		page = secretmem_alloc_page(ctx, vmf->gfp_mask);
		if (!page)
			return VM_FAULT_OOM;

		__SetPageUptodate(page);
		err = add_to_page_cache(page, mapping, offset, vmf->gfp_mask);
		if (unlikely(err)) {
			secretmem_free_page(ctx, page);
			put_page(page);
			if (err == -EEXIST)
				goto retry;
			return vmf_error(err);
		}

		set_page_private(page, (unsigned long)ctx);
	}

	vmf->page = page;
	return VM_FAULT_LOCKED;
}

static const struct vm_operations_struct secretmem_vm_ops = {
	.fault = secretmem_fault,
};

static int secretmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;

	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) == 0)
		return -EINVAL;

	if (mlock_future_check(vma->vm_mm, vma->vm_flags | VM_LOCKED, len))
		return -EAGAIN;

	vma->vm_ops = &secretmem_vm_ops;
	vma->vm_flags |= VM_LOCKED;

	return 0;
}

bool vma_is_secretmem(struct vm_area_struct *vma)
{
	return vma->vm_ops == &secretmem_vm_ops;
}

static const struct file_operations secretmem_fops = {
	.mmap		= secretmem_mmap,
};

static bool secretmem_isolate_page(struct page *page, isolate_mode_t mode)
{
	return false;
}

static int secretmem_migratepage(struct address_space *mapping,
				 struct page *newpage, struct page *page,
				 enum migrate_mode mode)
{
	return -EBUSY;
}

static void secretmem_freepage(struct page *page)
{
	struct secretmem_ctx *ctx = (struct secretmem_ctx *)page_private(page);

	secretmem_free_page(ctx, page);
}

static const struct address_space_operations secretmem_aops = {
	.freepage	= secretmem_freepage,
	.migratepage	= secretmem_migratepage,
	.isolate_page	= secretmem_isolate_page,
};

bool page_is_secretmem(struct page *page)
{
	struct address_space *mapping = page_mapping(page);

	if (!mapping)
		return false;

	return mapping->a_ops == &secretmem_aops;
}

static struct vfsmount *secretmem_mnt;

static struct file *secretmem_file_create(unsigned long flags)
{
	struct file *file = ERR_PTR(-ENOMEM);
	struct secretmem_ctx *ctx;
	struct inode *inode;

	inode = alloc_anon_inode(secretmem_mnt->mnt_sb);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		goto err_free_inode;

	ctx->pool = gen_pool_create(PAGE_SHIFT, NUMA_NO_NODE);
	if (!ctx->pool)
		goto err_free_ctx;

	file = alloc_file_pseudo(inode, secretmem_mnt, "secretmem",
				 O_RDWR, &secretmem_fops);
	if (IS_ERR(file))
		goto err_free_pool;

	mapping_set_unevictable(inode->i_mapping);

	inode->i_private = ctx;
	inode->i_mapping->private_data = ctx;
	inode->i_mapping->a_ops = &secretmem_aops;

	/* pretend we are a normal file with zero size */
	inode->i_mode |= S_IFREG;
	inode->i_size = 0;

	file->private_data = ctx;

	ctx->mode = flags & SECRETMEM_MODE_MASK;

	return file;

err_free_pool:
	gen_pool_destroy(ctx->pool);
err_free_ctx:
	kfree(ctx);
err_free_inode:
	iput(inode);
	return file;
}

SYSCALL_DEFINE1(memfd_secret, unsigned long, flags)
{
	struct file *file;
	int fd, err;

	/* make sure local flags do not confict with global fcntl.h */
	BUILD_BUG_ON(SECRETMEM_FLAGS_MASK & O_CLOEXEC);

	if (flags & ~(SECRETMEM_FLAGS_MASK | O_CLOEXEC))
		return -EINVAL;

	if (!secretmem_cma)
		return -ENOMEM;

	fd = get_unused_fd_flags(flags & O_CLOEXEC);
	if (fd < 0)
		return fd;

	file = secretmem_file_create(flags);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_put_fd;
	}

	file->f_flags |= O_LARGEFILE;

	fd_install(fd, file);
	return fd;

err_put_fd:
	put_unused_fd(fd);
	return err;
}

static void secretmem_cleanup_chunk(struct gen_pool *pool,
				    struct gen_pool_chunk *chunk, void *data)
{
	unsigned long start = chunk->start_addr;
	unsigned long end = chunk->end_addr;
	struct page *page = virt_to_page(start);
	unsigned long nr_pages = (end - start + 1) / PAGE_SIZE;
	int i;

	set_direct_map_default_noflush(page, nr_pages);
	secretmem_unaccount_pages(page, PMD_PAGE_ORDER);

	for (i = 0; i < nr_pages; i++)
		clear_highpage(page + i);

	cma_release(secretmem_cma, page, nr_pages);
}

static void secretmem_cleanup_pool(struct secretmem_ctx *ctx)
{
	struct gen_pool *pool = ctx->pool;

	gen_pool_for_each_chunk(pool, secretmem_cleanup_chunk, ctx);
	gen_pool_destroy(pool);
}

static void secretmem_evict_inode(struct inode *inode)
{
	struct secretmem_ctx *ctx = inode->i_private;

	truncate_inode_pages_final(&inode->i_data);
	secretmem_cleanup_pool(ctx);
	clear_inode(inode);
	kfree(ctx);
}

static const struct super_operations secretmem_super_ops = {
	.evict_inode = secretmem_evict_inode,
};

static int secretmem_init_fs_context(struct fs_context *fc)
{
	struct pseudo_fs_context *ctx = init_pseudo(fc, SECRETMEM_MAGIC);

	if (!ctx)
		return -ENOMEM;
	ctx->ops = &secretmem_super_ops;

	return 0;
}

static struct file_system_type secretmem_fs = {
	.name		= "secretmem",
	.init_fs_context = secretmem_init_fs_context,
	.kill_sb	= kill_anon_super,
};

static int secretmem_init(void)
{
	int ret = 0;

	secretmem_mnt = kern_mount(&secretmem_fs);
	if (IS_ERR(secretmem_mnt))
		ret = PTR_ERR(secretmem_mnt);

	return ret;
}
fs_initcall(secretmem_init);

static int __init secretmem_setup(char *str)
{
	phys_addr_t align = PMD_SIZE;
	unsigned long reserved_size;
	int err;

	reserved_size = memparse(str, NULL);
	if (!reserved_size)
		return 0;

	if (reserved_size * 2 > PUD_SIZE)
		align = PUD_SIZE;

	err = cma_declare_contiguous(0, reserved_size, 0, align, 0, false,
				     "secretmem", &secretmem_cma);
	if (err) {
		pr_err("failed to create CMA: %d\n", err);
		return err;
	}

	pr_info("reserved %luM\n", reserved_size >> 20);

	return 0;
}
__setup("secretmem=", secretmem_setup);
