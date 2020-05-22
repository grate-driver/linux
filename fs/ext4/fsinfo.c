// SPDX-License-Identifier: GPL-2.0
/* Filesystem information for ext4
 *
 * Copyright (C) 2020 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <linux/mount.h>
#include "ext4.h"

static int ext4_fsinfo_supports(struct path *path, struct fsinfo_context *ctx)
{
	struct fsinfo_supports *p = ctx->buffer;
	struct inode *inode = d_inode(path->dentry);
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_inode *raw_inode;
	u32 flags;

	fsinfo_generic_supports(path, ctx);
	p->stx_attributes |= (STATX_ATTR_APPEND |
			      STATX_ATTR_COMPRESSED |
			      STATX_ATTR_ENCRYPTED |
			      STATX_ATTR_IMMUTABLE |
			      STATX_ATTR_NODUMP |
			      STATX_ATTR_VERITY);
	if (EXT4_FITS_IN_INODE(raw_inode, ei, i_crtime))
		p->stx_mask |= STATX_BTIME;

	flags = EXT4_FL_USER_VISIBLE;
	if (S_ISREG(inode->i_mode))
		flags &= ~EXT4_PROJINHERIT_FL;
	p->fs_ioc_getflags = flags;
	flags &= EXT4_FL_USER_MODIFIABLE;
	p->fs_ioc_setflags_set = flags;
	p->fs_ioc_setflags_clear = flags;

	p->fs_ioc_fsgetxattr_xflags = EXT4_FL_XFLAG_VISIBLE;
	p->fs_ioc_fssetxattr_xflags_set = EXT4_FL_XFLAG_VISIBLE;
	p->fs_ioc_fssetxattr_xflags_clear = EXT4_FL_XFLAG_VISIBLE;
	return sizeof(*p);
}

static int ext4_fsinfo_features(struct path *path, struct fsinfo_context *ctx)
{
	struct fsinfo_features *p = ctx->buffer;
	struct super_block *sb = path->dentry->d_sb;
	struct inode *inode = d_inode(path->dentry);
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_inode *raw_inode;

	fsinfo_generic_features(path, ctx);
	fsinfo_set_unix_features(p);
	fsinfo_set_feature(p, FSINFO_FEAT_VOLUME_UUID);
	fsinfo_set_feature(p, FSINFO_FEAT_VOLUME_NAME);
	fsinfo_set_feature(p, FSINFO_FEAT_O_SYNC);
	fsinfo_set_feature(p, FSINFO_FEAT_O_DIRECT);
	fsinfo_set_feature(p, FSINFO_FEAT_ADV_LOCKS);

	if (test_opt(sb, XATTR_USER))
		fsinfo_set_feature(p, FSINFO_FEAT_XATTRS);
	if (ext4_has_feature_journal(sb))
		fsinfo_set_feature(p, FSINFO_FEAT_JOURNAL);
	if (ext4_has_feature_casefold(sb))
		fsinfo_set_feature(p, FSINFO_FEAT_NAME_CASE_INDEP);

	if (sb->s_flags & SB_I_VERSION &&
	    !test_opt2(sb, HURD_COMPAT) &&
	    EXT4_INODE_SIZE(sb) > EXT4_GOOD_OLD_INODE_SIZE) {
		fsinfo_set_feature(p, FSINFO_FEAT_IVER_DATA_CHANGE);
		fsinfo_set_feature(p, FSINFO_FEAT_IVER_MONO_INCR);
	}

	if (EXT4_FITS_IN_INODE(raw_inode, ei, i_crtime))
		fsinfo_set_feature(p, FSINFO_FEAT_HAS_BTIME);
	return sizeof(*p);
}

static int ext4_fsinfo_get_volume_name(struct path *path, struct fsinfo_context *ctx)
{
	const struct ext4_sb_info *sbi = EXT4_SB(path->mnt->mnt_sb);
	const struct ext4_super_block *es = sbi->s_es;

	memcpy(ctx->buffer, es->s_volume_name, sizeof(es->s_volume_name));
	return strlen(ctx->buffer) + 1;
}

static const struct fsinfo_attribute ext4_fsinfo_attributes[] = {
	FSINFO_VSTRUCT	(FSINFO_ATTR_SUPPORTS,		ext4_fsinfo_supports),
	FSINFO_VSTRUCT	(FSINFO_ATTR_FEATURES,		ext4_fsinfo_features),
	FSINFO_STRING	(FSINFO_ATTR_VOLUME_NAME,	ext4_fsinfo_get_volume_name),
	{}
};

int ext4_fsinfo(struct path *path, struct fsinfo_context *ctx)
{
	return fsinfo_get_attribute(path, ctx, ext4_fsinfo_attributes);
}
