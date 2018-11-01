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

/* setup.c */
extern int fsverity_file_open(struct inode *inode, struct file *filp);
extern int fsverity_prepare_setattr(struct dentry *dentry, struct iattr *attr);
extern int fsverity_prepare_getattr(struct inode *inode);
extern void fsverity_cleanup_inode(struct inode *inode);
extern loff_t fsverity_full_i_size(const struct inode *inode);

#else /* !__FS_HAS_VERITY */

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

#endif	/* !__FS_HAS_VERITY */

#endif	/* _LINUX_FSVERITY_H */
