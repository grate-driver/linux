/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* fsinfo() definitions.
 *
 * Copyright (C) 2020 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
#ifndef _UAPI_LINUX_FSINFO_H
#define _UAPI_LINUX_FSINFO_H

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/openat2.h>

/*
 * The filesystem attributes that can be requested.  Note that some attributes
 * may have multiple instances which can be switched in the parameter block.
 */
#define FSINFO_ATTR_STATFS		0x00	/* statfs()-style state */
#define FSINFO_ATTR_IDS			0x01	/* Filesystem IDs */
#define FSINFO_ATTR_LIMITS		0x02	/* Filesystem limits */
#define FSINFO_ATTR_SUPPORTS		0x03	/* What's supported in statx, iocflags, ... */
#define FSINFO_ATTR_TIMESTAMP_INFO	0x04	/* Inode timestamp info */
#define FSINFO_ATTR_VOLUME_ID		0x05	/* Volume ID (string) */
#define FSINFO_ATTR_VOLUME_UUID		0x06	/* Volume UUID (LE uuid) */
#define FSINFO_ATTR_VOLUME_NAME		0x07	/* Volume name (string) */
#define FSINFO_ATTR_FEATURES		0x08	/* Filesystem features (bits) */
#define FSINFO_ATTR_SOURCE		0x09	/* Superblock source/device name (string) */
#define FSINFO_ATTR_CONFIGURATION	0x0a	/* Superblock configuration/options (string) */
#define FSINFO_ATTR_FS_STATISTICS	0x0b	/* Superblock filesystem statistics (string) */

#define FSINFO_ATTR_FSINFO_ATTRIBUTE_INFO 0x100	/* Information about attr N (for path) */
#define FSINFO_ATTR_FSINFO_ATTRIBUTES	0x101	/* List of supported attrs (for path) */

#define FSINFO_ATTR_MOUNT_INFO		0x200	/* Mount object information */
#define FSINFO_ATTR_MOUNT_PATH		0x201	/* Bind mount/superblock path (string) */
#define FSINFO_ATTR_MOUNT_POINT		0x202	/* Relative path of mount in parent (string) */
#define FSINFO_ATTR_MOUNT_POINT_FULL	0x203	/* Absolute path of mount (string) */
#define FSINFO_ATTR_MOUNT_TOPOLOGY	0x204	/* Mount object topology */
#define FSINFO_ATTR_MOUNT_CHILDREN	0x205	/* Children of this mount (list) */

#define FSINFO_ATTR_AFS_CELL_NAME	0x300	/* AFS cell name (string) */
#define FSINFO_ATTR_AFS_SERVER_NAME	0x301	/* Name of the Nth server (string) */
#define FSINFO_ATTR_AFS_SERVER_ADDRESSES 0x302	/* List of addresses of the Nth server */

/*
 * Optional fsinfo() parameter structure.
 *
 * If this is not given, it is assumed that fsinfo_attr_statfs instance 0,0 is
 * desired.
 */
struct fsinfo_params {
	__u64	resolve_flags;	/* RESOLVE_* flags */
	__u32	at_flags;	/* AT_* flags */
	__u32	flags;		/* Flags controlling fsinfo() specifically */
#define FSINFO_FLAGS_QUERY_MASK	0x0007 /* What object should fsinfo() query? */
#define FSINFO_FLAGS_QUERY_PATH	0x0000 /* - path, specified by dirfd,pathname,AT_EMPTY_PATH */
#define FSINFO_FLAGS_QUERY_FD	0x0001 /* - fd specified by dirfd */
#define FSINFO_FLAGS_QUERY_MOUNT 0x0002	/* - mount object (path=>mount_id, dirfd=>subtree) */
	__u32	request;	/* ID of requested attribute */
	__u32	Nth;		/* Instance of it (some may have multiple) */
	__u32	Mth;		/* Subinstance of Nth instance */
};

enum fsinfo_value_type {
	FSINFO_TYPE_VSTRUCT	= 0,	/* Version-lengthed struct (up to 4096 bytes) */
	FSINFO_TYPE_STRING	= 1,	/* NUL-term var-length string (up to 4095 chars) */
	FSINFO_TYPE_OPAQUE	= 2,	/* Opaque blob (unlimited size) */
	FSINFO_TYPE_LIST	= 3,	/* List of ints/structs (unlimited size) */
};

/*
 * Information struct for fsinfo(FSINFO_ATTR_FSINFO_ATTRIBUTE_INFO).
 *
 * This gives information about the attributes supported by fsinfo for the
 * given path.
 */
struct fsinfo_attribute_info {
	unsigned int		attr_id;	/* The ID of the attribute */
	enum fsinfo_value_type	type;		/* The type of the attribute's value(s) */
	unsigned int		flags;
#define FSINFO_FLAGS_N		0x01		/* - Attr has a set of values */
#define FSINFO_FLAGS_NM		0x02		/* - Attr has a set of sets of values */
	unsigned int		size;		/* - Value size (FSINFO_STRUCT/FSINFO_LIST) */
};

#define FSINFO_ATTR_FSINFO_ATTRIBUTE_INFO__STRUCT struct fsinfo_attribute_info
#define FSINFO_ATTR_FSINFO_ATTRIBUTES__STRUCT __u32

struct fsinfo_u128 {
#if defined(__BYTE_ORDER) ? __BYTE_ORDER == __BIG_ENDIAN : defined(__BIG_ENDIAN)
	__u64	hi;
	__u64	lo;
#elif defined(__BYTE_ORDER) ? __BYTE_ORDER == __LITTLE_ENDIAN : defined(__LITTLE_ENDIAN)
	__u64	lo;
	__u64	hi;
#endif
};

/*
 * Information struct for fsinfo(FSINFO_ATTR_MOUNT_INFO).
 */
struct fsinfo_mount_info {
	__u64	sb_unique_id;		/* Kernel-lifetime unique superblock ID */
	__u64	mnt_unique_id;		/* Kernel-lifetime unique mount ID */
	__u32	mnt_id;			/* Mount identifier (use with AT_FSINFO_MOUNTID_PATH) */
	__u32	attr;			/* MOUNT_ATTR_* flags */
	__u32	sb_changes;		/* Number of sb configuration changes */
	__u32	sb_notifications;	/* Number of other sb notifications */
	__u32	mnt_attr_changes;	/* Number of attribute changes to this mount. */
	__u32	mnt_topology_changes;	/* Number of topology changes to this mount. */
	__u32	mnt_subtree_notifications; /* Number of notifications in mount subtree */
	__u32	padding[1];
};

#define FSINFO_ATTR_MOUNT_INFO__STRUCT struct fsinfo_mount_info

/*
 * Information struct for fsinfo(FSINFO_ATTR_MOUNT_TOPOLOGY).
 */
struct fsinfo_mount_topology {
	__u32	parent_id;		/* Parent mount identifier */
	__u32	group_id;		/* Mount group ID */
	__u32	master_id;		/* Slave master group ID */
	__u32	from_id;		/* Slave propagated from ID */
	__u32	propagation;		/* MOUNT_PROPAGATION_* flags */
	__u32	mnt_topology_changes;	/* Number of topology changes to this mount. */
};

#define FSINFO_ATTR_MOUNT_TOPOLOGY__STRUCT struct fsinfo_mount_topology

/*
 * Information struct element for fsinfo(FSINFO_ATTR_MOUNT_CHILDREN).
 * - An extra element is placed on the end representing the parent mount.
 */
struct fsinfo_mount_child {
	__u64	mnt_unique_id;		/* Kernel-lifetime unique mount ID */
	__u32	mnt_id;			/* Mount identifier (use with AT_FSINFO_MOUNTID_PATH) */
	__u32	notify_sum;		/* Sum of sb_changes, sb_notifications, mnt_attr_changes,
					 * mnt_topology_changes and mnt_subtree_notifications.
					 */
};

#define FSINFO_ATTR_MOUNT_CHILDREN__STRUCT struct fsinfo_mount_child

/*
 * Information struct for fsinfo(FSINFO_ATTR_STATFS).
 * - This gives extended filesystem information.
 */
struct fsinfo_statfs {
	struct fsinfo_u128 f_blocks;	/* Total number of blocks in fs */
	struct fsinfo_u128 f_bfree;	/* Total number of free blocks */
	struct fsinfo_u128 f_bavail;	/* Number of free blocks available to ordinary user */
	struct fsinfo_u128 f_files;	/* Total number of file nodes in fs */
	struct fsinfo_u128 f_ffree;	/* Number of free file nodes */
	struct fsinfo_u128 f_favail;	/* Number of file nodes available to ordinary user */
	__u64	f_bsize;		/* Optimal block size */
	__u64	f_frsize;		/* Fragment size */
};

#define FSINFO_ATTR_STATFS__STRUCT struct fsinfo_statfs

/*
 * Information struct for fsinfo(FSINFO_ATTR_IDS).
 *
 * List of basic identifiers as is normally found in statfs().
 */
struct fsinfo_ids {
	char	f_fs_name[15 + 1];	/* Filesystem name */
	__u64	f_fsid;			/* Short 64-bit Filesystem ID (as statfs) */
	__u64	f_sb_id;		/* Internal superblock ID for sbnotify()/mntnotify() */
	__u32	f_fstype;		/* Filesystem type from linux/magic.h [uncond] */
	__u32	f_dev_major;		/* As st_dev_* from struct statx [uncond] */
	__u32	f_dev_minor;
	__u32	__padding[1];
};

#define FSINFO_ATTR_IDS__STRUCT struct fsinfo_ids

/*
 * Information struct for fsinfo(FSINFO_ATTR_LIMITS).
 *
 * List of supported filesystem limits.
 */
struct fsinfo_limits {
	struct fsinfo_u128 max_file_size;	/* Maximum file size */
	struct fsinfo_u128 max_ino;		/* Maximum inode number */
	__u64	max_uid;			/* Maximum UID supported */
	__u64	max_gid;			/* Maximum GID supported */
	__u64	max_projid;			/* Maximum project ID supported */
	__u64	max_hard_links;			/* Maximum number of hard links on a file */
	__u64	max_xattr_body_len;		/* Maximum xattr content length */
	__u32	max_xattr_name_len;		/* Maximum xattr name length */
	__u32	max_filename_len;		/* Maximum filename length */
	__u32	max_symlink_len;		/* Maximum symlink content length */
	__u32	max_dev_major;			/* Maximum device major representable */
	__u32	max_dev_minor;			/* Maximum device minor representable */
	__u32	__padding[1];
};

#define FSINFO_ATTR_LIMITS__STRUCT struct fsinfo_limits

/*
 * Information struct for fsinfo(FSINFO_ATTR_SUPPORTS).
 *
 * What's supported in various masks, such as statx() attribute and mask bits
 * and IOC flags.
 */
struct fsinfo_supports {
	__u64	stx_attributes;		/* What statx::stx_attributes are supported */
	__u32	stx_mask;		/* What statx::stx_mask bits are supported */
	__u32	fs_ioc_getflags;	/* What FS_IOC_GETFLAGS may return */
	__u32	fs_ioc_setflags_set;	/* What FS_IOC_SETFLAGS may set */
	__u32	fs_ioc_setflags_clear;	/* What FS_IOC_SETFLAGS may clear */
	__u32	fs_ioc_fsgetxattr_xflags; /* What FS_IOC_FSGETXATTR[A] may return in fsx_xflags */
	__u32	fs_ioc_fssetxattr_xflags_set; /* What FS_IOC_FSSETXATTR may set in fsx_xflags */
	__u32	fs_ioc_fssetxattr_xflags_clear; /* What FS_IOC_FSSETXATTR may set in fsx_xflags */
	__u32	win_file_attrs;		/* What DOS/Windows FILE_* attributes are supported */
};

#define FSINFO_ATTR_SUPPORTS__STRUCT struct fsinfo_supports

/*
 * Information struct for fsinfo(FSINFO_ATTR_FEATURES).
 *
 * Bitmask indicating filesystem features where renderable as single bits.
 */
enum fsinfo_feature {
	FSINFO_FEAT_IS_KERNEL_FS	= 0,	/* fs is kernel-special filesystem */
	FSINFO_FEAT_IS_BLOCK_FS		= 1,	/* fs is block-based filesystem */
	FSINFO_FEAT_IS_FLASH_FS		= 2,	/* fs is flash filesystem */
	FSINFO_FEAT_IS_NETWORK_FS	= 3,	/* fs is network filesystem */
	FSINFO_FEAT_IS_AUTOMOUNTER_FS	= 4,	/* fs is automounter special filesystem */
	FSINFO_FEAT_IS_MEMORY_FS	= 5,	/* fs is memory-based filesystem */
	FSINFO_FEAT_AUTOMOUNTS		= 6,	/* fs supports automounts */
	FSINFO_FEAT_ADV_LOCKS		= 7,	/* fs supports advisory file locking */
	FSINFO_FEAT_MAND_LOCKS		= 8,	/* fs supports mandatory file locking */
	FSINFO_FEAT_LEASES		= 9,	/* fs supports file leases */
	FSINFO_FEAT_UIDS		= 10,	/* fs supports numeric uids */
	FSINFO_FEAT_GIDS		= 11,	/* fs supports numeric gids */
	FSINFO_FEAT_PROJIDS		= 12,	/* fs supports numeric project ids */
	FSINFO_FEAT_STRING_USER_IDS	= 13,	/* fs supports string user identifiers */
	FSINFO_FEAT_GUID_USER_IDS	= 14,	/* fs supports GUID user identifiers */
	FSINFO_FEAT_WINDOWS_ATTRS	= 15,	/* fs has windows attributes */
	FSINFO_FEAT_USER_QUOTAS		= 16,	/* fs has per-user quotas */
	FSINFO_FEAT_GROUP_QUOTAS	= 17,	/* fs has per-group quotas */
	FSINFO_FEAT_PROJECT_QUOTAS	= 18,	/* fs has per-project quotas */
	FSINFO_FEAT_XATTRS		= 19,	/* fs has xattrs */
	FSINFO_FEAT_JOURNAL		= 20,	/* fs has a journal */
	FSINFO_FEAT_DATA_IS_JOURNALLED	= 21,	/* fs is using data journalling */
	FSINFO_FEAT_O_SYNC		= 22,	/* fs supports O_SYNC */
	FSINFO_FEAT_O_DIRECT		= 23,	/* fs supports O_DIRECT */
	FSINFO_FEAT_VOLUME_ID		= 24,	/* fs has a volume ID */
	FSINFO_FEAT_VOLUME_UUID		= 25,	/* fs has a volume UUID */
	FSINFO_FEAT_VOLUME_NAME		= 26,	/* fs has a volume name */
	FSINFO_FEAT_VOLUME_FSID		= 27,	/* fs has a volume FSID */
	FSINFO_FEAT_IVER_ALL_CHANGE	= 28,	/* i_version represents data + meta changes */
	FSINFO_FEAT_IVER_DATA_CHANGE	= 29,	/* i_version represents data changes only */
	FSINFO_FEAT_IVER_MONO_INCR	= 30,	/* i_version incremented monotonically */
	FSINFO_FEAT_DIRECTORIES		= 31,	/* fs supports (sub)directories */
	FSINFO_FEAT_SYMLINKS		= 32,	/* fs supports symlinks */
	FSINFO_FEAT_HARD_LINKS		= 33,	/* fs supports hard links */
	FSINFO_FEAT_HARD_LINKS_1DIR	= 34,	/* fs supports hard links in same dir only */
	FSINFO_FEAT_DEVICE_FILES	= 35,	/* fs supports bdev, cdev */
	FSINFO_FEAT_UNIX_SPECIALS	= 36,	/* fs supports pipe, fifo, socket */
	FSINFO_FEAT_RESOURCE_FORKS	= 37,	/* fs supports resource forks/streams */
	FSINFO_FEAT_NAME_CASE_INDEP	= 38,	/* Filename case independence is mandatory */
	FSINFO_FEAT_NAME_CASE_FOLD	= 39,	/* Filename case is folded on medium */
	FSINFO_FEAT_NAME_NON_UTF8	= 40,	/* fs has non-utf8 names */
	FSINFO_FEAT_NAME_HAS_CODEPAGE	= 41,	/* fs has a filename codepage */
	FSINFO_FEAT_SPARSE		= 42,	/* fs supports sparse files */
	FSINFO_FEAT_NOT_PERSISTENT	= 43,	/* fs is not persistent */
	FSINFO_FEAT_NO_UNIX_MODE	= 44,	/* fs does not support unix mode bits */
	FSINFO_FEAT_HAS_ATIME		= 45,	/* fs supports access time */
	FSINFO_FEAT_HAS_BTIME		= 46,	/* fs supports birth/creation time */
	FSINFO_FEAT_HAS_CTIME		= 47,	/* fs supports change time */
	FSINFO_FEAT_HAS_MTIME		= 48,	/* fs supports modification time */
	FSINFO_FEAT_HAS_ACL		= 49,	/* fs supports ACLs of some sort */
	FSINFO_FEAT_HAS_INODE_NUMBERS	= 50,	/* fs has inode numbers */
	FSINFO_FEAT__NR
};

struct fsinfo_features {
	__u32	nr_features;	/* Number of supported features (FSINFO_FEAT__NR) */
	__u8	features[(FSINFO_FEAT__NR + 7) / 8];
};

#define FSINFO_ATTR_FEATURES__STRUCT struct fsinfo_features

struct fsinfo_timestamp_one {
	__s64	minimum;	/* Minimum timestamp value in seconds */
	__s64	maximum;	/* Maximum timestamp value in seconds */
	__u16	gran_mantissa;	/* Granularity(secs) = mant * 10^exp */
	__s8	gran_exponent;
	__u8	__padding[5];
};

/*
 * Information struct for fsinfo(FSINFO_ATTR_TIMESTAMP_INFO).
 */
struct fsinfo_timestamp_info {
	struct fsinfo_timestamp_one	atime;	/* Access time */
	struct fsinfo_timestamp_one	mtime;	/* Modification time */
	struct fsinfo_timestamp_one	ctime;	/* Change time */
	struct fsinfo_timestamp_one	btime;	/* Birth/creation time */
};

#define FSINFO_ATTR_TIMESTAMP_INFO__STRUCT struct fsinfo_timestamp_info

/*
 * Information struct for fsinfo(FSINFO_ATTR_VOLUME_UUID).
 */
struct fsinfo_volume_uuid {
	__u8	uuid[16];
};

#define FSINFO_ATTR_VOLUME_UUID__STRUCT struct fsinfo_volume_uuid

/*
 * Information struct for fsinfo(FSINFO_ATTR_AFS_SERVER_ADDRESSES).
 *
 * Get the addresses of the Nth server for a network filesystem.
 */
struct fsinfo_afs_server_address {
	struct __kernel_sockaddr_storage address;
};

#define FSINFO_ATTR_AFS_SERVER_ADDRESSES__STRUCT struct fsinfo_afs_server_address

#endif /* _UAPI_LINUX_FSINFO_H */
