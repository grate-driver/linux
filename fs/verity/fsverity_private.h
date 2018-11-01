/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fs-verity: read-only file-based authenticity protection
 *
 * Copyright 2018 Google LLC
 */

#ifndef _FSVERITY_PRIVATE_H
#define _FSVERITY_PRIVATE_H

#ifdef CONFIG_FS_VERITY_DEBUG
#define DEBUG
#endif

#define pr_fmt(fmt) "fs-verity: " fmt

#include <crypto/sha.h>
#define __FS_HAS_VERITY 1
#include <linux/fsverity.h>

/*
 * Maximum depth of the Merkle tree.  Up to 64 levels are theoretically possible
 * with a very small block size, but we'd like to limit stack usage during
 * verification, and in practice this is plenty.  E.g., with SHA-256 and 4K
 * blocks, a file with size UINT64_MAX bytes needs just 8 levels.
 */
#define FS_VERITY_MAX_LEVELS		16

/*
 * Largest digest size among all hash algorithms supported by fs-verity.  This
 * can be increased if needed.
 */
#define FS_VERITY_MAX_DIGEST_SIZE	SHA512_DIGEST_SIZE

/* A hash algorithm supported by fs-verity */
struct fsverity_hash_alg {
	struct crypto_ahash *tfm; /* allocated on demand */
	const char *name;
	unsigned int digest_size;
	bool cryptographic;
};

/**
 * fsverity_info - cached verity metadata for an inode
 *
 * When a verity file is first opened, an instance of this struct is allocated
 * and stored in ->i_verity_info.  It caches various values from the verity
 * metadata, such as the tree topology and the root hash, which are needed to
 * efficiently verify data read from the file.  Once created, it remains until
 * the inode is evicted.
 *
 * (The tree pages themselves are not cached here, though they may be cached in
 * the inode's page cache.)
 */
struct fsverity_info {
	const struct fsverity_hash_alg *hash_alg; /* hash algorithm */
	u8 block_bits;			/* log2(block size) */
	u8 log_arity;			/* log2(hashes per hash block) */
	u8 depth;			/* num levels in the Merkle tree */
	u8 *hashstate;			/* salted initial hash state */
	loff_t data_i_size;		/* original file size */
	loff_t metadata_end;		/* offset to end of verity metadata */
	u8 root_hash[FS_VERITY_MAX_DIGEST_SIZE];   /* Merkle tree root hash */
	u8 measurement[FS_VERITY_MAX_DIGEST_SIZE]; /* file measurement */
	bool have_root_hash;		/* have root hash from disk? */
	bool have_signed_measurement;	/* have measurement from signature? */

	/* Starting blocks for each tree level. 'depth-1' is the root level. */
	u64 hash_lvl_region_idx[FS_VERITY_MAX_LEVELS];
};

/* hash_algs.c */
extern struct fsverity_hash_alg fsverity_hash_algs[];
const struct fsverity_hash_alg *fsverity_get_hash_alg(unsigned int num);
void __init fsverity_check_hash_algs(void);
void __exit fsverity_exit_hash_algs(void);

/* setup.c */
struct page *fsverity_read_metadata_page(struct inode *inode, pgoff_t index);
struct fsverity_info *create_fsverity_info(struct inode *inode, bool enabling);
void free_fsverity_info(struct fsverity_info *vi);

static inline struct fsverity_info *get_fsverity_info(const struct inode *inode)
{
	/* pairs with cmpxchg_release() in set_fsverity_info() */
	return smp_load_acquire(&inode->i_verity_info);
}

static inline bool set_fsverity_info(struct inode *inode,
				     struct fsverity_info *vi)
{
	/* Make sure the in-memory i_size is set to the data i_size */
	i_size_write(inode, vi->data_i_size);

	/* pairs with smp_load_acquire() in get_fsverity_info() */
	return cmpxchg_release(&inode->i_verity_info, NULL, vi) == NULL;
}

/* signature.c */
#ifdef CONFIG_FS_VERITY_BUILTIN_SIGNATURES
extern int fsverity_require_signatures;

int fsverity_parse_pkcs7_signature_extension(struct fsverity_info *vi,
					     const void *raw_pkcs7,
					     size_t size);

int __init fsverity_signature_init(void);

void __exit fsverity_signature_exit(void);
#else /* CONFIG_FS_VERITY_BUILTIN_SIGNATURES */

#define fsverity_require_signatures 0

static inline int
fsverity_parse_pkcs7_signature_extension(struct fsverity_info *vi,
					 const void *raw_pkcs7, size_t size)
{
	pr_warn("PKCS#7 signatures not supported in this kernel build!\n");
	return -EINVAL;
}

static inline int fsverity_signature_init(void)
{
	return 0;
}

static inline void fsverity_signature_exit(void)
{
}
#endif /* !CONFIG_FS_VERITY_BUILTIN_SIGNATURES */

/* verify.c */
extern struct workqueue_struct *fsverity_read_workqueue;

#endif /* _FSVERITY_PRIVATE_H */
