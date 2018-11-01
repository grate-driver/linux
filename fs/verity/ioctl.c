// SPDX-License-Identifier: GPL-2.0
/*
 * fs/verity/ioctl.c: fs-verity ioctls
 *
 * Copyright 2018 Google LLC
 *
 * Originally written by Jaegeuk Kim and Michael Halcrow;
 * heavily rewritten by Eric Biggers.
 */

#include "fsverity_private.h"

#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/uaccess.h>

/**
 * fsverity_ioctl_enable - enable fs-verity on a file
 *
 * Enable fs-verity on a file.  Verity metadata must have already been appended
 * to the file.  See Documentation/filesystems/fsverity.rst, section
 * 'FS_IOC_ENABLE_VERITY' for details.
 *
 * Return: 0 on success, -errno on failure
 */
int fsverity_ioctl_enable(struct file *filp, const void __user *arg)
{
	struct inode *inode = file_inode(filp);
	struct fsverity_info *vi;
	int err;

	err = inode_permission(inode, MAY_WRITE);
	if (err)
		return err;

	if (IS_APPEND(inode))
		return -EPERM;

	if (arg) /* argument is reserved */
		return -EINVAL;

	if (S_ISDIR(inode->i_mode))
		return -EISDIR;

	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	err = mnt_want_write_file(filp);
	if (err)
		goto out;

	/*
	 * Temporarily lock out writers via writable file descriptors or
	 * truncate().  This should stabilize the contents of the file as well
	 * as its size.  Note that at the end of this ioctl we will unlock
	 * writers, but at that point the verity bit will be set (if the ioctl
	 * succeeded), preventing future writers.
	 */
	err = deny_write_access(filp);
	if (err) /* -ETXTBSY */
		goto out_drop_write;

	/*
	 * fsync so that the verity bit can't be persisted to disk prior to the
	 * data, causing verification errors after a crash.
	 */
	err = vfs_fsync(filp, 1);
	if (err)
		goto out_allow_write;

	/* Serialize concurrent use of this ioctl on the same inode */
	inode_lock(inode);

	if (get_fsverity_info(inode)) { /* fs-verity already enabled? */
		err = -EEXIST;
		goto out_unlock;
	}

	/* Validate the verity metadata */
	vi = create_fsverity_info(inode, true);
	if (IS_ERR(vi)) {
		err = PTR_ERR(vi);
		if (err == -EINVAL) /* distinguish "invalid metadata" case */
			err = -EBADMSG;
		goto out_unlock;
	}

	/*
	 * Ask the filesystem to mark the file as a verity file, e.g. by setting
	 * the verity bit in the inode.
	 */
	err = inode->i_sb->s_vop->set_verity(inode, vi->data_i_size);
	if (err)
		goto out_free_vi;

	/* Invalidate all cached pages, forcing re-verification */
	truncate_inode_pages(inode->i_mapping, 0);

	/*
	 * Set ->i_verity_info, unless another task managed to do it already
	 * between ->set_verity() and here.
	 */
	if (set_fsverity_info(inode, vi))
		vi = NULL;
	err = 0;
out_free_vi:
	free_fsverity_info(vi);
out_unlock:
	inode_unlock(inode);
out_allow_write:
	allow_write_access(filp);
out_drop_write:
	mnt_drop_write_file(filp);
out:
	return err;
}
EXPORT_SYMBOL_GPL(fsverity_ioctl_enable);
