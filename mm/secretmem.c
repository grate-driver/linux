// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright IBM Corporation, 2020
 *
 * Author: Mike Rapoport <rppt@linux.ibm.com>
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/memfd.h>
#include <linux/bitops.h>
#include <linux/printk.h>
#include <linux/pagemap.h>
#include <linux/genalloc.h>
#include <linux/syscalls.h>
#include <linux/pseudo_fs.h>
#include <linux/set_memory.h>
#include <linux/sched/signal.h>

#include <uapi/linux/secretmem.h>
#include <uapi/linux/magic.h>

#include <asm/tlbflush.h>

#include "internal.h"

#undef pr_fmt
#define pr_fmt(fmt) "secretmem: " fmt

/*
 * Secret memory areas are always exclusive to owning mm and they are
 * removed from the direct map.
 */
#ifdef CONFIG_HAVE_SECRETMEM_UNCACHED
#define SECRETMEM_MODE_MASK	(SECRETMEM_UNCACHED)
#else
#define SECRETMEM_MODE_MASK	(0x0)
#endif

#define SECRETMEM_FLAGS_MASK	SECRETMEM_MODE_MASK

struct secretmem_ctx {
	struct gen_pool *pool;
	unsigned int mode;
};

static int secretmem_pool_increase(struct secretmem_ctx *ctx, gfp_t gfp)
{
	unsigned long nr_pages = (1 << PMD_PAGE_ORDER);
	struct gen_pool *pool = ctx->pool;
	unsigned long addr;
	struct page *page;
	int err;

	page = alloc_pages(gfp, PMD_PAGE_ORDER);
	if (!page)
		return -ENOMEM;

	addr = (unsigned long)page_address(page);
	split_page(page, PMD_PAGE_ORDER);

	err = gen_pool_add(pool, addr, PMD_SIZE, NUMA_NO_NODE);
	if (err) {
		__free_pages(page, PMD_PAGE_ORDER);
		return err;
	}

	__kernel_map_pages(page, nr_pages, 0);

	return 0;
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
	int ret = 0;

	if (((loff_t)vmf->pgoff << PAGE_SHIFT) >= i_size_read(inode))
		return vmf_error(-EINVAL);

	page = find_get_entry(mapping, offset);
	if (!page) {
		page = secretmem_alloc_page(ctx, vmf->gfp_mask);
		if (!page)
			return vmf_error(-ENOMEM);

		ret = add_to_page_cache(page, mapping, offset, vmf->gfp_mask);
		if (unlikely(ret))
			goto err_put_page;

		__SetPageUptodate(page);
		set_page_private(page, (unsigned long)ctx);

		ret = VM_FAULT_LOCKED;
	}

	vmf->page = page;
	return ret;

err_put_page:
	put_page(page);
	return vmf_error(ret);
}

static const struct vm_operations_struct secretmem_vm_ops = {
	.fault = secretmem_fault,
};

static int secretmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct secretmem_ctx *ctx = file->private_data;
	unsigned long len = vma->vm_end - vma->vm_start;

	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) == 0)
		return -EINVAL;

	if (mlock_future_check(vma->vm_mm, vma->vm_flags | VM_LOCKED, len))
		return -EAGAIN;

	if (ctx->mode & SECRETMEM_UNCACHED)
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	vma->vm_ops = &secretmem_vm_ops;
	vma->vm_flags |= VM_LOCKED;

	return 0;
}

const struct file_operations secretmem_fops = {
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
	unsigned long addr = (unsigned long)page_address(page);
	struct secretmem_ctx *ctx = (struct secretmem_ctx *)page_private(page);
	struct gen_pool *pool = ctx->pool;

	gen_pool_free(pool, addr, PAGE_SIZE);
}

static const struct address_space_operations secretmem_aops = {
	.freepage	= secretmem_freepage,
	.migratepage	= secretmem_migratepage,
	.isolate_page	= secretmem_isolate_page,
};

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
	unsigned long nr_pages, addr;

	nr_pages = (end - start + 1) / PAGE_SIZE;
	__kernel_map_pages(virt_to_page(start), nr_pages, 1);

	for (addr = start; addr < end; addr += PAGE_SIZE)
		put_page(virt_to_page(addr));
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
