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
#include <linux/syscalls.h>
#include <linux/pseudo_fs.h>
#include <linux/secretmem.h>
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
	unsigned int mode;
};

static struct page *secretmem_alloc_page(gfp_t gfp)
{
	/*
	 * FIXME: use a cache of large pages to reduce the direct map
	 * fragmentation
	 */
	return alloc_page(gfp | __GFP_ZERO);
}

static vm_fault_t secretmem_fault(struct vm_fault *vmf)
{
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	pgoff_t offset = vmf->pgoff;
	unsigned long addr;
	struct page *page;
	int err;

	if (((loff_t)vmf->pgoff << PAGE_SHIFT) >= i_size_read(inode))
		return vmf_error(-EINVAL);

retry:
	page = find_lock_page(mapping, offset);
	if (!page) {
		page = secretmem_alloc_page(vmf->gfp_mask);
		if (!page)
			return VM_FAULT_OOM;

		err = set_direct_map_invalid_noflush(page, 1);
		if (err) {
			put_page(page);
			return vmf_error(err);
		}

		__SetPageUptodate(page);
		err = add_to_page_cache(page, mapping, offset, vmf->gfp_mask);
		if (unlikely(err)) {
			put_page(page);
			if (err == -EEXIST)
				goto retry;
			goto err_restore_direct_map;
		}

		addr = (unsigned long)page_address(page);
		flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
	}

	vmf->page = page;
	return VM_FAULT_LOCKED;

err_restore_direct_map:
	/*
	 * If a split of large page was required, it already happened
	 * when we marked the page invalid which guarantees that this call
	 * won't fail
	 */
	set_direct_map_default_noflush(page, 1);
	return vmf_error(err);
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
	set_direct_map_default_noflush(page, 1);
	clear_highpage(page);
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

	file = alloc_file_pseudo(inode, secretmem_mnt, "secretmem",
				 O_RDWR, &secretmem_fops);
	if (IS_ERR(file))
		goto err_free_ctx;

	mapping_set_unevictable(inode->i_mapping);

	inode->i_mapping->private_data = ctx;
	inode->i_mapping->a_ops = &secretmem_aops;

	/* pretend we are a normal file with zero size */
	inode->i_mode |= S_IFREG;
	inode->i_size = 0;

	file->private_data = ctx;

	ctx->mode = flags & SECRETMEM_MODE_MASK;

	return file;

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

static void secretmem_evict_inode(struct inode *inode)
{
	struct secretmem_ctx *ctx = inode->i_private;

	truncate_inode_pages_final(&inode->i_data);
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
