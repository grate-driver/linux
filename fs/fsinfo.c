// SPDX-License-Identifier: GPL-2.0
/* Filesystem information query.
 *
 * Copyright (C) 2020 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
#include <linux/syscalls.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/security.h>
#include <linux/uaccess.h>
#include <linux/fsinfo.h>
#include <uapi/linux/mount.h>
#include "internal.h"

/**
 * fsinfo_opaque - Store opaque blob as an fsinfo attribute value.
 * @s: The blob to store (may be NULL)
 * @ctx: The parameter context
 * @len: The length of the blob
 */
int fsinfo_opaque(const void *s, struct fsinfo_context *ctx, unsigned int len)
{
	void *p = ctx->buffer;
	int ret = 0;

	if (s) {
		if (!ctx->want_size_only)
			memcpy(p, s, len);
		ret = len;
	}

	return ret;
}
EXPORT_SYMBOL(fsinfo_opaque);

/**
 * fsinfo_string - Store a NUL-terminated string as an fsinfo attribute value.
 * @s: The string to store (may be NULL)
 * @ctx: The parameter context
 */
int fsinfo_string(const char *s, struct fsinfo_context *ctx)
{
	if (!s)
		return 1;
	return fsinfo_opaque(s, ctx, min_t(size_t, strlen(s) + 1, ctx->buf_size));
}
EXPORT_SYMBOL(fsinfo_string);

/*
 * Get basic filesystem stats from statfs.
 */
static int fsinfo_generic_statfs(struct path *path, struct fsinfo_context *ctx)
{
	struct fsinfo_statfs *p = ctx->buffer;
	struct kstatfs buf;
	int ret;

	ret = vfs_statfs(path, &buf);
	if (ret < 0)
		return ret;

	p->f_blocks.lo	= buf.f_blocks;
	p->f_bfree.lo	= buf.f_bfree;
	p->f_bavail.lo	= buf.f_bavail;
	p->f_files.lo	= buf.f_files;
	p->f_ffree.lo	= buf.f_ffree;
	p->f_favail.lo	= buf.f_ffree;
	p->f_bsize	= buf.f_bsize;
	p->f_frsize	= buf.f_frsize;
	return sizeof(*p);
}

static int fsinfo_generic_ids(struct path *path, struct fsinfo_context *ctx)
{
	struct fsinfo_ids *p = ctx->buffer;
	struct super_block *sb;
	struct kstatfs buf;
	int ret;

	ret = vfs_statfs(path, &buf);
	if (ret < 0 && ret != -ENOSYS)
		return ret;
	if (ret == 0)
		memcpy(&p->f_fsid, &buf.f_fsid, sizeof(p->f_fsid));

	sb = path->dentry->d_sb;
	p->f_fstype	= sb->s_magic;
	p->f_dev_major	= MAJOR(sb->s_dev);
	p->f_dev_minor	= MINOR(sb->s_dev);
	p->f_sb_id	= sb->s_unique_id;
	strlcpy(p->f_fs_name, sb->s_type->name, sizeof(p->f_fs_name));
	return sizeof(*p);
}

int fsinfo_generic_limits(struct path *path, struct fsinfo_context *ctx)
{
	struct fsinfo_limits *p = ctx->buffer;
	struct super_block *sb = path->dentry->d_sb;

	p->max_file_size.hi	= 0;
	p->max_file_size.lo	= sb->s_maxbytes;
	p->max_ino.hi		= 0;
	p->max_ino.lo		= UINT_MAX;
	p->max_hard_links	= sb->s_max_links;
	p->max_uid		= UINT_MAX;
	p->max_gid		= UINT_MAX;
	p->max_projid		= UINT_MAX;
	p->max_filename_len	= NAME_MAX;
	p->max_symlink_len	= PATH_MAX;
	p->max_xattr_name_len	= XATTR_NAME_MAX;
	p->max_xattr_body_len	= XATTR_SIZE_MAX;
	p->max_dev_major	= 0xffffff;
	p->max_dev_minor	= 0xff;
	return sizeof(*p);
}
EXPORT_SYMBOL(fsinfo_generic_limits);

int fsinfo_generic_supports(struct path *path, struct fsinfo_context *ctx)
{
	struct fsinfo_supports *p = ctx->buffer;
	struct super_block *sb = path->dentry->d_sb;

	p->stx_mask = STATX_BASIC_STATS;
	if (sb->s_d_op && sb->s_d_op->d_automount)
		p->stx_attributes |= STATX_ATTR_AUTOMOUNT;
	return sizeof(*p);
}
EXPORT_SYMBOL(fsinfo_generic_supports);

static const struct fsinfo_timestamp_info fsinfo_default_timestamp_info = {
	.atime = {
		.minimum	= S64_MIN,
		.maximum	= S64_MAX,
		.gran_mantissa	= 1,
		.gran_exponent	= 0,
	},
	.mtime = {
		.minimum	= S64_MIN,
		.maximum	= S64_MAX,
		.gran_mantissa	= 1,
		.gran_exponent	= 0,
	},
	.ctime = {
		.minimum	= S64_MIN,
		.maximum	= S64_MAX,
		.gran_mantissa	= 1,
		.gran_exponent	= 0,
	},
	.btime = {
		.minimum	= S64_MIN,
		.maximum	= S64_MAX,
		.gran_mantissa	= 1,
		.gran_exponent	= 0,
	},
};

int fsinfo_generic_timestamp_info(struct path *path, struct fsinfo_context *ctx)
{
	struct fsinfo_timestamp_info *p = ctx->buffer;
	struct super_block *sb = path->dentry->d_sb;
	s8 exponent;

	*p = fsinfo_default_timestamp_info;

	if (sb->s_time_gran < 1000000000) {
		if (sb->s_time_gran < 1000)
			exponent = -9;
		else if (sb->s_time_gran < 1000000)
			exponent = -6;
		else
			exponent = -3;

		p->atime.gran_exponent = exponent;
		p->mtime.gran_exponent = exponent;
		p->ctime.gran_exponent = exponent;
		p->btime.gran_exponent = exponent;
	}

	return sizeof(*p);
}
EXPORT_SYMBOL(fsinfo_generic_timestamp_info);

static int fsinfo_generic_volume_uuid(struct path *path, struct fsinfo_context *ctx)
{
	struct fsinfo_volume_uuid *p = ctx->buffer;
	struct super_block *sb = path->dentry->d_sb;

	memcpy(p, &sb->s_uuid, sizeof(*p));
	return sizeof(*p);
}

static int fsinfo_generic_volume_id(struct path *path, struct fsinfo_context *ctx)
{
	return fsinfo_string(path->dentry->d_sb->s_id, ctx);
}

static const struct fsinfo_attribute fsinfo_common_attributes[] = {
	FSINFO_VSTRUCT	(FSINFO_ATTR_STATFS,		fsinfo_generic_statfs),
	FSINFO_VSTRUCT	(FSINFO_ATTR_IDS,		fsinfo_generic_ids),
	FSINFO_VSTRUCT	(FSINFO_ATTR_LIMITS,		fsinfo_generic_limits),
	FSINFO_VSTRUCT	(FSINFO_ATTR_SUPPORTS,		fsinfo_generic_supports),
	FSINFO_VSTRUCT	(FSINFO_ATTR_TIMESTAMP_INFO,	fsinfo_generic_timestamp_info),
	FSINFO_STRING	(FSINFO_ATTR_VOLUME_ID,		fsinfo_generic_volume_id),
	FSINFO_VSTRUCT	(FSINFO_ATTR_VOLUME_UUID,	fsinfo_generic_volume_uuid),

	FSINFO_LIST	(FSINFO_ATTR_FSINFO_ATTRIBUTES,	(void *)123UL),
	FSINFO_VSTRUCT_N(FSINFO_ATTR_FSINFO_ATTRIBUTE_INFO, (void *)123UL),
	{}
};

/*
 * Determine an attribute's minimum buffer size and, if the buffer is large
 * enough, get the attribute value.
 */
static int fsinfo_get_this_attribute(struct path *path,
				     struct fsinfo_context *ctx,
				     const struct fsinfo_attribute *attr)
{
	int buf_size;

	if (ctx->Nth != 0 && !(attr->flags & (FSINFO_FLAGS_N | FSINFO_FLAGS_NM)))
		return -ENODATA;
	if (ctx->Mth != 0 && !(attr->flags & FSINFO_FLAGS_NM))
		return -ENODATA;

	switch (attr->type) {
	case FSINFO_TYPE_VSTRUCT:
		ctx->clear_tail = true;
		buf_size = attr->size;
		break;
	case FSINFO_TYPE_STRING:
	case FSINFO_TYPE_OPAQUE:
	case FSINFO_TYPE_LIST:
		buf_size = 4096;
		break;
	default:
		return -ENOPKG;
	}

	if (ctx->buf_size < buf_size)
		return buf_size;

	return attr->get(path, ctx);
}

static void fsinfo_attributes_insert(struct fsinfo_context *ctx,
				     const struct fsinfo_attribute *attr)
{
	__u32 *p = ctx->buffer;
	unsigned int i;

	if (ctx->usage >= ctx->buf_size ||
	    ctx->buf_size - ctx->usage < sizeof(__u32)) {
		ctx->usage += sizeof(__u32);
		return;
	}

	for (i = 0; i < ctx->usage / sizeof(__u32); i++)
		if (p[i] == attr->attr_id)
			return;

	p[i] = attr->attr_id;
	ctx->usage += sizeof(__u32);
}

static int fsinfo_list_attributes(struct path *path,
				  struct fsinfo_context *ctx,
				  const struct fsinfo_attribute *attributes)
{
	const struct fsinfo_attribute *a;

	for (a = attributes; a->get; a++)
		fsinfo_attributes_insert(ctx, a);
	return -EOPNOTSUPP; /* We want to go through all the lists */
}

static int fsinfo_get_attribute_info(struct path *path,
				     struct fsinfo_context *ctx,
				     const struct fsinfo_attribute *attributes)
{
	const struct fsinfo_attribute *a;
	struct fsinfo_attribute_info *p = ctx->buffer;

	if (!ctx->buf_size)
		return sizeof(*p);

	for (a = attributes; a->get; a++) {
		if (a->attr_id == ctx->Nth) {
			p->attr_id	= a->attr_id;
			p->type		= a->type;
			p->flags	= a->flags;
			p->size		= a->size;
			p->size		= a->size;
			return sizeof(*p);
		}
	}
	return -EOPNOTSUPP; /* We want to go through all the lists */
}

/**
 * fsinfo_get_attribute - Look up and handle an attribute
 * @path: The object to query
 * @params: Parameters to define a request and place to store result
 * @attributes: List of attributes to search.
 *
 * Look through a list of attributes for one that matches the requested
 * attribute then call the handler for it.
 */
int fsinfo_get_attribute(struct path *path, struct fsinfo_context *ctx,
			 const struct fsinfo_attribute *attributes)
{
	const struct fsinfo_attribute *a;

	switch (ctx->requested_attr) {
	case FSINFO_ATTR_FSINFO_ATTRIBUTE_INFO:
		return fsinfo_get_attribute_info(path, ctx, attributes);
	case FSINFO_ATTR_FSINFO_ATTRIBUTES:
		return fsinfo_list_attributes(path, ctx, attributes);
	default:
		for (a = attributes; a->get; a++)
			if (a->attr_id == ctx->requested_attr)
				return fsinfo_get_this_attribute(path, ctx, a);
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL(fsinfo_get_attribute);

/**
 * generic_fsinfo - Handle an fsinfo attribute generically
 * @path: The object to query
 * @params: Parameters to define a request and place to store result
 */
static int fsinfo_call(struct path *path, struct fsinfo_context *ctx)
{
	int ret;

	if (path->dentry->d_sb->s_op->fsinfo) {
		ret = path->dentry->d_sb->s_op->fsinfo(path, ctx);
		if (ret != -EOPNOTSUPP)
			return ret;
	}
	ret = fsinfo_get_attribute(path, ctx, fsinfo_common_attributes);
	if (ret != -EOPNOTSUPP)
		return ret;

	switch (ctx->requested_attr) {
	case FSINFO_ATTR_FSINFO_ATTRIBUTE_INFO:
		return -ENODATA;
	case FSINFO_ATTR_FSINFO_ATTRIBUTES:
		return ctx->usage;
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * vfs_fsinfo - Retrieve filesystem information
 * @path: The object to query
 * @params: Parameters to define a request and place to store result
 *
 * Get an attribute on a filesystem or an object within a filesystem.  The
 * filesystem attribute to be queried is indicated by @ctx->requested_attr, and
 * if it's a multi-valued attribute, the particular value is selected by
 * @ctx->Nth and then @ctx->Mth.
 *
 * For common attributes, a value may be fabricated if it is not supported by
 * the filesystem.
 *
 * On success, the size of the attribute's value is returned (0 is a valid
 * size).  A buffer will have been allocated and will be pointed to by
 * @ctx->buffer.  The caller must free this with kvfree().
 *
 * Errors can also be returned: -ENOMEM if a buffer cannot be allocated, -EPERM
 * or -EACCES if permission is denied by the LSM, -EOPNOTSUPP if an attribute
 * doesn't exist for the specified object or -ENODATA if the attribute exists,
 * but the Nth,Mth value does not exist.  -EMSGSIZE indicates that the value is
 * unmanageable internally and -ENOPKG indicates other internal failure.
 *
 * Errors such as -EIO may also come from attempts to access media or servers
 * to obtain the requested information if it's not immediately to hand.
 *
 * [*] Note that the caller may set @ctx->want_size_only if it only wants the
 *     size of the value and not the data.  If this is set, a buffer may not be
 *     allocated under some circumstances.  This is intended for size query by
 *     userspace.
 *
 * [*] Note that @ctx->clear_tail will be returned set if the data should be
 *     padded out with zeros when writing it to userspace.
 */
static int vfs_fsinfo(struct path *path, struct fsinfo_context *ctx)
{
	struct dentry *dentry = path->dentry;
	int ret;

	ret = security_sb_statfs(dentry);
	if (ret)
		return ret;

	/* Call the handler to find out the buffer size required. */
	ctx->buf_size = 0;
	ret = fsinfo_call(path, ctx);
	if (ret < 0 || ctx->want_size_only)
		return ret;
	ctx->buf_size = ret;

	do {
		/* Allocate a buffer of the requested size. */
		if (ctx->buf_size > INT_MAX)
			return -EMSGSIZE;
		ctx->buffer = kvzalloc(ctx->buf_size, GFP_KERNEL);
		if (!ctx->buffer)
			return -ENOMEM;

		ctx->usage = 0;
		ctx->skip = 0;
		ret = fsinfo_call(path, ctx);
		if (IS_ERR_VALUE((long)ret))
			return ret;
		if ((unsigned int)ret <= ctx->buf_size)
			return ret; /* It fitted */

		/* We need to resize the buffer */
		ctx->buf_size = roundup(ret, PAGE_SIZE);
		kvfree(ctx->buffer);
		ctx->buffer = NULL;
	} while (!signal_pending(current));

	return -ERESTARTSYS;
}

static int vfs_fsinfo_path(int dfd, const char __user *pathname,
			   const struct fsinfo_params *up,
			   struct fsinfo_context *ctx)
{
	struct path path;
	unsigned lookup_flags = LOOKUP_FOLLOW | LOOKUP_AUTOMOUNT;
	int ret = -EINVAL;

	if (up->resolve_flags & ~VALID_RESOLVE_FLAGS)
		return -EINVAL;
	if (up->at_flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT |
			     AT_EMPTY_PATH))
		return -EINVAL;

	if (up->resolve_flags & RESOLVE_NO_XDEV)
		lookup_flags |= LOOKUP_NO_XDEV;
	if (up->resolve_flags & RESOLVE_NO_MAGICLINKS)
		lookup_flags |= LOOKUP_NO_MAGICLINKS;
	if (up->resolve_flags & RESOLVE_NO_SYMLINKS)
		lookup_flags |= LOOKUP_NO_SYMLINKS;
	if (up->resolve_flags & RESOLVE_BENEATH)
		lookup_flags |= LOOKUP_BENEATH;
	if (up->resolve_flags & RESOLVE_IN_ROOT)
		lookup_flags |= LOOKUP_IN_ROOT;
	if (up->at_flags & AT_SYMLINK_NOFOLLOW)
		lookup_flags &= ~LOOKUP_FOLLOW;
	if (up->at_flags & AT_NO_AUTOMOUNT)
		lookup_flags &= ~LOOKUP_AUTOMOUNT;
	if (up->at_flags & AT_EMPTY_PATH)
		lookup_flags |= LOOKUP_EMPTY;

retry:
	ret = user_path_at(dfd, pathname, lookup_flags, &path);
	if (ret)
		goto out;

	ret = vfs_fsinfo(&path, ctx);
	path_put(&path);
	if (retry_estale(ret, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	return ret;
}

static int vfs_fsinfo_fd(unsigned int fd, struct fsinfo_context *ctx)
{
	struct fd f = fdget_raw(fd);
	int ret = -EBADF;

	if (f.file) {
		ret = vfs_fsinfo(&f.file->f_path, ctx);
		fdput(f);
	}
	return ret;
}

/**
 * sys_fsinfo - System call to get filesystem information
 * @dfd: Base directory to pathwalk from or fd referring to filesystem.
 * @pathname: Filesystem to query or NULL.
 * @params: Parameters to define request (NULL: FSINFO_ATTR_STATFS).
 * @params_size: Size of parameter buffer.
 * @result_buffer: Result buffer.
 * @result_buf_size: Size of result buffer.
 *
 * Get information on a filesystem.  The filesystem attribute to be queried is
 * indicated by @_params->request, and some of the attributes can have multiple
 * values, indexed by @_params->Nth and @_params->Mth.  If @_params is NULL,
 * then the 0th fsinfo_attr_statfs attribute is queried.  If an attribute does
 * not exist, EOPNOTSUPP is returned; if the Nth,Mth value does not exist,
 * ENODATA is returned.
 *
 * On success, the size of the attribute's value is returned.  If
 * @result_buf_size is 0 or @result_buffer is NULL, only the size is returned.
 * If the size of the value is larger than @result_buf_size, it will be
 * truncated by the copy.  If the size of the value is smaller than
 * @result_buf_size then the excess buffer space will be cleared.  The full
 * size of the value will be returned, irrespective of how much data is
 * actually placed in the buffer.
 */
SYSCALL_DEFINE6(fsinfo,
		int, dfd,
		const char __user *, pathname,
		const struct fsinfo_params __user *, params,
		size_t, params_size,
		void __user *, result_buffer,
		size_t, result_buf_size)
{
	struct fsinfo_context ctx;
	struct fsinfo_params user_params;
	unsigned int result_size;
	void *r;
	int ret;

	if ((!params &&  params_size) ||
	    ( params && !params_size) ||
	    (!result_buffer &&  result_buf_size) ||
	    ( result_buffer && !result_buf_size))
		return -EINVAL;
	if (result_buf_size > UINT_MAX)
		return -EOVERFLOW;

	memset(&ctx, 0, sizeof(ctx));
	ctx.requested_attr	= FSINFO_ATTR_STATFS;
	ctx.flags		= FSINFO_FLAGS_QUERY_PATH;
	ctx.want_size_only	= (result_buf_size == 0);

	if (params) {
		ret = copy_struct_from_user(&user_params, sizeof(user_params),
					    params, params_size);
		if (ret < 0)
			return ret;
		if (user_params.flags & ~FSINFO_FLAGS_QUERY_MASK)
			return -EINVAL;
		ctx.flags = user_params.flags;
		ctx.requested_attr = user_params.request;
		ctx.Nth = user_params.Nth;
		ctx.Mth = user_params.Mth;
	}

	switch (ctx.flags & FSINFO_FLAGS_QUERY_MASK) {
	case FSINFO_FLAGS_QUERY_PATH:
		ret = vfs_fsinfo_path(dfd, pathname, &user_params, &ctx);
		break;
	case FSINFO_FLAGS_QUERY_FD:
		if (pathname)
			return -EINVAL;
		ret = vfs_fsinfo_fd(dfd, &ctx);
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		goto error;

	r = ctx.buffer + ctx.skip;
	result_size = min_t(size_t, ret, result_buf_size);
	if (result_size > 0 &&
	    copy_to_user(result_buffer, r, result_size) != 0) {
		ret = -EFAULT;
		goto error;
	}

	/* Clear any part of the buffer that we won't fill if we're putting a
	 * struct in there.  Strings, opaque objects and arrays are expected to
	 * be variable length.
	 */
	if (ctx.clear_tail &&
	    result_buf_size > result_size &&
	    clear_user(result_buffer + result_size,
		       result_buf_size - result_size) != 0) {
		ret = -EFAULT;
		goto error;
	}

error:
	kvfree(ctx.buffer);
	return ret;
}
