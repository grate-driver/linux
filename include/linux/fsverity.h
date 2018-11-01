/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fs-verity: read-only file-based authenticity protection
 *
 * Copyright 2018 Google LLC
 */

#ifndef _LINUX_FSVERITY_H
#define _LINUX_FSVERITY_H

#include <linux/fs.h>
#include <uapi/linux/fsverity.h>

/*
 * fs-verity operations for filesystems
 */
struct fsverity_operations {
	int (*set_verity)(struct inode *inode, loff_t data_i_size);
	int (*get_metadata_end)(struct inode *inode, loff_t *metadata_end_ret);
};

#if __FS_HAS_VERITY

/* ioctl.c */
extern int fsverity_ioctl_enable(struct file *filp, const void __user *arg);
extern int fsverity_ioctl_measure(struct file *filp, void __user *arg);

/* setup.c */
extern int fsverity_file_open(struct inode *inode, struct file *filp);
extern int fsverity_prepare_setattr(struct dentry *dentry, struct iattr *attr);
extern int fsverity_prepare_getattr(struct inode *inode);
extern void fsverity_cleanup_inode(struct inode *inode);
extern loff_t fsverity_full_i_size(const struct inode *inode);

/* verify.c */
extern bool fsverity_verify_page(struct page *page);
extern void fsverity_verify_bio(struct bio *bio);
extern void fsverity_enqueue_verify_work(struct work_struct *work);

static inline bool fsverity_check_hole(struct inode *inode, struct page *page)
{
	return inode->i_verity_info == NULL || fsverity_verify_page(page);
}

#else /* !__FS_HAS_VERITY */

/* ioctl.c */

static inline int fsverity_ioctl_enable(struct file *filp,
					const void __user *arg)
{
	return -EOPNOTSUPP;
}

static inline int fsverity_ioctl_measure(struct file *filp, void __user *arg)
{
	return -EOPNOTSUPP;
}

/* setup.c */

static inline int fsverity_file_open(struct inode *inode, struct file *filp)
{
	return -EOPNOTSUPP;
}

static inline int fsverity_prepare_setattr(struct dentry *dentry,
					   struct iattr *attr)
{
	return -EOPNOTSUPP;
}

static inline int fsverity_prepare_getattr(struct inode *inode)
{
	return -EOPNOTSUPP;
}

static inline void fsverity_cleanup_inode(struct inode *inode)
{
}

static inline loff_t fsverity_full_i_size(const struct inode *inode)
{
	return i_size_read(inode);
}

/* verify.c */

static inline bool fsverity_verify_page(struct page *page)
{
	WARN_ON(1);
	return false;
}

static inline void fsverity_verify_bio(struct bio *bio)
{
	WARN_ON(1);
}

static inline void fsverity_enqueue_verify_work(struct work_struct *work)
{
	WARN_ON(1);
}

static inline bool fsverity_check_hole(struct inode *inode, struct page *page)
{
	return true;
}

#endif	/* !__FS_HAS_VERITY */

#endif	/* _LINUX_FSVERITY_H */
