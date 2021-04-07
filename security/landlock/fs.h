/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock LSM - Filesystem management and hooks
 *
 * Copyright © 2017-2020 Mickaël Salaün <mic@digikod.net>
 * Copyright © 2018-2020 ANSSI
 */

#ifndef _SECURITY_LANDLOCK_FS_H
#define _SECURITY_LANDLOCK_FS_H

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/rcupdate.h>

#include "ruleset.h"
#include "setup.h"

struct landlock_inode_security {
	/*
	 * @object: Weak pointer to an allocated object.  All writes (i.e.
	 * creating a new object or removing one) are protected by the
	 * underlying inode->i_lock.  Disassociating @object from the inode is
	 * additionally protected by @object->lock, from the time @object's
	 * usage refcount drops to zero to the time this pointer is nulled out.
	 * Cf. release_inode().
	 */
	struct landlock_object __rcu *object;
};

struct landlock_superblock_security {
	/*
	 * @inode_refs: References to Landlock underlying objects.
	 * Cf. struct super_block->s_fsnotify_inode_refs .
	 */
	atomic_long_t inode_refs;
};

static inline struct landlock_inode_security *landlock_inode(
		const struct inode *const inode)
{
	return inode->i_security + landlock_blob_sizes.lbs_inode;
}

static inline struct landlock_superblock_security *landlock_superblock(
		const struct super_block *const superblock)
{
	return superblock->s_security + landlock_blob_sizes.lbs_superblock;
}

__init void landlock_add_fs_hooks(void);

int landlock_append_fs_rule(struct landlock_ruleset *const ruleset,
		const struct path *const path, u32 access_hierarchy);

#endif /* _SECURITY_LANDLOCK_FS_H */
