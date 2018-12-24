/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * fs-verity (file-based verity) support
 *
 * Copyright (C) 2018 Google LLC
 */
#ifndef _UAPI_LINUX_FSVERITY_H
#define _UAPI_LINUX_FSVERITY_H

#include <linux/limits.h>
#include <linux/ioctl.h>
#include <linux/types.h>

/* ========== Ioctls ========== */

struct fsverity_digest {
	__u16 digest_algorithm;
	__u16 digest_size; /* input/output */
	__u8 digest[];
};

#define FS_IOC_ENABLE_VERITY	_IO('f', 133)
#define FS_IOC_MEASURE_VERITY	_IOWR('f', 134, struct fsverity_digest)

/* ========== On-disk format ========== */

#define FS_VERITY_MAGIC		"FSVerity"

/* Supported hash algorithms */
#define FS_VERITY_ALG_SHA256	1
#define FS_VERITY_ALG_SHA512	2
#define FS_VERITY_ALG_CRC32C	3	/* for integrity only */

/* Metadata stored near the end of verity files, after the Merkle tree */
/* This structure is 64 bytes long */
struct fsverity_descriptor {
	__u8 magic[8];		/* must be FS_VERITY_MAGIC */
	__u8 major_version;	/* must be 1 */
	__u8 minor_version;	/* must be 0 */
	__u8 log_data_blocksize;/* log2(data-bytes-per-hash), e.g. 12 for 4KB */
	__u8 log_tree_blocksize;/* log2(tree-bytes-per-hash), e.g. 12 for 4KB */
	__le16 data_algorithm;	/* hash algorithm for data blocks */
	__le16 tree_algorithm;	/* hash algorithm for tree blocks */
	__le32 flags;		/* flags */
	__le32 reserved1;	/* must be 0 */
	__le64 orig_file_size;	/* size of the original file data */
	__le16 auth_ext_count;	/* number of authenticated extensions */
	__u8 reserved2[30];	/* must be 0 */
};
/* followed by list of 'auth_ext_count' authenticated extensions */
/*
 * then followed by '__le16 unauth_ext_count' padded to next 8-byte boundary,
 * then a list of 'unauth_ext_count' (may be 0) unauthenticated extensions
 */

/* Extension types */
#define FS_VERITY_EXT_ROOT_HASH		1
#define FS_VERITY_EXT_SALT		2
#define FS_VERITY_EXT_PKCS7_SIGNATURE	3

/* Header of each extension (variable-length metadata item) */
struct fsverity_extension {
	/*
	 * Length in bytes, including this header but excluding padding to next
	 * 8-byte boundary that is applied when advancing to the next extension.
	 */
	__le32 length;
	__le16 type;		/* Type of this extension (see codes above) */
	__le16 reserved;	/* Reserved, must be 0 */
};
/* followed by the payload of 'length - 8' bytes */

/* Extension payload formats */

/*
 * FS_VERITY_EXT_ROOT_HASH payload is just a byte array, with size equal to the
 * digest size of the hash algorithm given in the fsverity_descriptor
 */

/* FS_VERITY_EXT_SALT payload is just a byte array, any size */

/*
 * FS_VERITY_EXT_PKCS7_SIGNATURE payload is a DER-encoded PKCS#7 message
 * containing the signed file measurement in the following format:
 */
struct fsverity_digest_disk {
	__le16 digest_algorithm;
	__le16 digest_size;
	__u8 digest[];
};

/* Fields stored at the very end of the file */
struct fsverity_footer {
	__le32 desc_reverse_offset;	/* distance to fsverity_descriptor */
	__u8 magic[8];			/* FS_VERITY_MAGIC */
} __packed;

#endif /* _UAPI_LINUX_FSVERITY_H */
