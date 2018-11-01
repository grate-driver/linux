// SPDX-License-Identifier: GPL-2.0
/*
 * fs/verity/setup.c: fs-verity module initialization and descriptor parsing
 *
 * Copyright 2018 Google LLC
 *
 * Originally written by Jaegeuk Kim and Michael Halcrow;
 * heavily rewritten by Eric Biggers.
 */

#include "fsverity_private.h"

#include <crypto/hash.h>
#include <linux/highmem.h>
#include <linux/list_sort.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>

static struct kmem_cache *fsverity_info_cachep;

static void dump_fsverity_descriptor(const struct fsverity_descriptor *desc)
{
	pr_debug("magic = %.*s\n", (int)sizeof(desc->magic), desc->magic);
	pr_debug("major_version = %u\n", desc->major_version);
	pr_debug("minor_version = %u\n", desc->minor_version);
	pr_debug("log_data_blocksize = %u\n", desc->log_data_blocksize);
	pr_debug("log_tree_blocksize = %u\n", desc->log_tree_blocksize);
	pr_debug("data_algorithm = %u\n", le16_to_cpu(desc->data_algorithm));
	pr_debug("tree_algorithm = %u\n", le16_to_cpu(desc->tree_algorithm));
	pr_debug("flags = %#x\n", le32_to_cpu(desc->flags));
	pr_debug("orig_file_size = %llu\n", le64_to_cpu(desc->orig_file_size));
	pr_debug("auth_ext_count = %u\n", le16_to_cpu(desc->auth_ext_count));
}

/* Precompute the salted initial hash state */
static int set_salt(struct fsverity_info *vi, const u8 *salt, size_t saltlen)
{
	struct crypto_ahash *tfm = vi->hash_alg->tfm;
	struct ahash_request *req;
	unsigned int reqsize = sizeof(*req) + crypto_ahash_reqsize(tfm);
	struct scatterlist sg;
	DECLARE_CRYPTO_WAIT(wait);
	u8 *saltbuf;
	int err;

	vi->hashstate = kmalloc(crypto_ahash_statesize(tfm), GFP_KERNEL);
	if (!vi->hashstate)
		return -ENOMEM;
	/* On error, vi->hashstate is freed by free_fsverity_info() */

	/*
	 * Allocate a hash request buffer.  Also reserve space for a copy of
	 * the salt, since the given 'salt' may point into vmap'ed memory, so
	 * sg_init_one() may not work on it.
	 */
	req = kmalloc(reqsize + saltlen, GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	saltbuf = (u8 *)req + reqsize;
	memcpy(saltbuf, salt, saltlen);
	sg_init_one(&sg, saltbuf, saltlen);

	ahash_request_set_tfm(req, tfm);
	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_SLEEP |
				   CRYPTO_TFM_REQ_MAY_BACKLOG,
				   crypto_req_done, &wait);
	ahash_request_set_crypt(req, &sg, NULL, saltlen);

	err = crypto_wait_req(crypto_ahash_init(req), &wait);
	if (err)
		goto out;
	err = crypto_wait_req(crypto_ahash_update(req), &wait);
	if (err)
		goto out;
	err = crypto_ahash_export(req, vi->hashstate);
out:
	kfree(req);
	return err;
}

/*
 * Copy in the root hash stored on disk.
 *
 * Note that the root hash could be computed by hashing the root block of the
 * Merkle tree.  But it works out a bit simpler to store the hash separately;
 * then it gets included in the file measurement without special-casing it, and
 * the root block gets verified on the ->readpages() path like the other blocks.
 */
static int parse_root_hash_extension(struct fsverity_info *vi,
				     const void *hash, size_t size)
{
	const struct fsverity_hash_alg *alg = vi->hash_alg;

	if (vi->have_root_hash) {
		pr_warn("Multiple root hashes were found!\n");
		return -EINVAL;
	}
	if (size != alg->digest_size) {
		pr_warn("Wrong root hash size; got %zu bytes, but expected %u for hash algorithm %s\n",
			size, alg->digest_size, alg->name);
		return -EINVAL;
	}
	memcpy(vi->root_hash, hash, size);
	vi->have_root_hash = true;
	pr_debug("Root hash: %s:%*phN\n", alg->name,
		 alg->digest_size, vi->root_hash);
	return 0;
}

static int parse_salt_extension(struct fsverity_info *vi,
				const void *salt, size_t saltlen)
{
	if (vi->hashstate) {
		pr_warn("Multiple salts were found!\n");
		return -EINVAL;
	}
	return set_salt(vi, salt, saltlen);
}

/* The available types of extensions (variable-length metadata items) */
static const struct extension_type {
	int (*parse)(struct fsverity_info *vi, const void *_ext,
		     size_t extra_len);
	size_t base_len;      /* length of fixed-size part of payload, if any */
	bool unauthenticated; /* true if not included in file measurement */
} extension_types[] = {
	[FS_VERITY_EXT_ROOT_HASH] = {
		.parse = parse_root_hash_extension,
	},
	[FS_VERITY_EXT_SALT] = {
		.parse = parse_salt_extension,
	},
	[FS_VERITY_EXT_PKCS7_SIGNATURE] = {
		.parse = fsverity_parse_pkcs7_signature_extension,
		.unauthenticated = true,
	},
};

static int do_parse_extensions(struct fsverity_info *vi,
			       const struct fsverity_extension **ext_hdr_p,
			       const void *end, int count, bool authenticated)
{
	const struct fsverity_extension *ext_hdr = *ext_hdr_p;
	int i;
	int err;

	for (i = 0; i < count; i++) {
		const struct extension_type *type;
		u32 len, rounded_len;
		u16 type_code;

		if (end - (const void *)ext_hdr < sizeof(*ext_hdr)) {
			pr_warn("Extension list overflows buffer\n");
			return -EINVAL;
		}
		type_code = le16_to_cpu(ext_hdr->type);
		if (type_code >= ARRAY_SIZE(extension_types) ||
		    !extension_types[type_code].parse) {
			pr_warn("Unknown extension type: %u\n", type_code);
			return -EINVAL;
		}
		type = &extension_types[type_code];
		if (authenticated != !type->unauthenticated) {
			pr_warn("Extension type %u must be %sauthenticated\n",
				type_code, type->unauthenticated ? "un" : "");
			return -EINVAL;
		}
		if (ext_hdr->reserved) {
			pr_warn("Reserved bits set in extension header\n");
			return -EINVAL;
		}
		len = le32_to_cpu(ext_hdr->length);
		if (len < sizeof(*ext_hdr)) {
			pr_warn("Invalid length in extension header\n");
			return -EINVAL;
		}
		rounded_len = round_up(len, 8);
		if (rounded_len == 0 ||
		    rounded_len > end - (const void *)ext_hdr) {
			pr_warn("Extension item overflows buffer\n");
			return -EINVAL;
		}
		if (len < sizeof(*ext_hdr) + type->base_len) {
			pr_warn("Extension length too small for type\n");
			return -EINVAL;
		}
		err = type->parse(vi, ext_hdr + 1,
				  len - sizeof(*ext_hdr) - type->base_len);
		if (err)
			return err;
		ext_hdr = (const void *)ext_hdr + rounded_len;
	}
	*ext_hdr_p = ext_hdr;
	return 0;
}

/*
 * Parse the extension items following the fixed-size portion of the fs-verity
 * descriptor.  The fsverity_info is updated accordingly.
 *
 * Return: On success, the size of the authenticated portion of the descriptor
 *	   (the fixed-size portion plus the authenticated extensions).
 *	   Otherwise, a -errno value.
 */
static int parse_extensions(struct fsverity_info *vi,
			    const struct fsverity_descriptor *desc,
			    int desc_len)
{
	const struct fsverity_extension *ext_hdr = (const void *)(desc + 1);
	const void *end = (const void *)desc + desc_len;
	u16 auth_ext_count = le16_to_cpu(desc->auth_ext_count);
	int auth_desc_len;
	int err;

	/* Authenticated extensions */
	err = do_parse_extensions(vi, &ext_hdr, end, auth_ext_count, true);
	if (err)
		return err;
	auth_desc_len = (void *)ext_hdr - (void *)desc;

	/*
	 * Unauthenticated extensions (optional).  Careful: an attacker able to
	 * corrupt the file can change these arbitrarily without being detected.
	 * Thus, only specific types of extensions are whitelisted here --
	 * namely, the ones containing a signature of the file measurement,
	 * which by definition can't be included in the file measurement itself.
	 */
	if (end - (void *)ext_hdr >= 8) {
		u16 unauth_ext_count = le16_to_cpup((__le16 *)ext_hdr);

		ext_hdr = (void *)ext_hdr + 8;
		err = do_parse_extensions(vi, &ext_hdr, end,
					  unauth_ext_count, false);
		if (err)
			return err;
	}

	return auth_desc_len;
}

/*
 * Parse an fs-verity descriptor, loading information into the fsverity_info.
 *
 * Return: On success, the size of the authenticated portion of the descriptor
 *	   (the fixed-size portion plus the authenticated extensions).
 *	   Otherwise, a -errno value.
 */
static int parse_fsverity_descriptor(struct fsverity_info *vi,
				     const struct fsverity_descriptor *desc,
				     int desc_len)
{
	unsigned int alg_num;
	unsigned int hashes_per_block;
	int desc_auth_len;
	int err;

	BUILD_BUG_ON(sizeof(*desc) != 64);

	/* magic */
	if (memcmp(desc->magic, FS_VERITY_MAGIC, sizeof(desc->magic))) {
		pr_warn("Wrong magic bytes\n");
		return -EINVAL;
	}

	/* major_version */
	if (desc->major_version != 1) {
		pr_warn("Unsupported major version (%u)\n",
			desc->major_version);
		return -EINVAL;
	}

	/* minor_version */
	if (desc->minor_version != 0) {
		pr_warn("Unsupported minor version (%u)\n",
			desc->minor_version);
		return -EINVAL;
	}

	/* data_algorithm and tree_algorithm */
	alg_num = le16_to_cpu(desc->data_algorithm);
	if (alg_num != le16_to_cpu(desc->tree_algorithm)) {
		pr_warn("Unimplemented case: data (%u) and tree (%u) hash algorithms differ\n",
			alg_num, le16_to_cpu(desc->tree_algorithm));
		return -EINVAL;
	}
	vi->hash_alg = fsverity_get_hash_alg(alg_num);
	if (IS_ERR(vi->hash_alg))
		return PTR_ERR(vi->hash_alg);

	/* log_data_blocksize and log_tree_blocksize */
	if (desc->log_data_blocksize != PAGE_SHIFT) {
		pr_warn("Unsupported log_blocksize (%u).  Need block_size == PAGE_SIZE.\n",
			desc->log_data_blocksize);
		return -EINVAL;
	}
	if (desc->log_tree_blocksize != desc->log_data_blocksize) {
		pr_warn("Unimplemented case: data (%u) and tree (%u) block sizes differ\n",
			desc->log_data_blocksize, desc->log_data_blocksize);
		return -EINVAL;
	}
	vi->block_bits = desc->log_data_blocksize;
	hashes_per_block = (1 << vi->block_bits) / vi->hash_alg->digest_size;
	if (!is_power_of_2(hashes_per_block)) {
		pr_warn("Unimplemented case: hashes per block (%u) isn't a power of 2\n",
			hashes_per_block);
		return -EINVAL;
	}
	vi->log_arity = ilog2(hashes_per_block);

	/* flags */
	if (desc->flags) {
		pr_warn("Unsupported flags (%#x)\n", le32_to_cpu(desc->flags));
		return -EINVAL;
	}

	/* reserved fields */
	if (desc->reserved1 ||
	    memchr_inv(desc->reserved2, 0, sizeof(desc->reserved2))) {
		pr_warn("Reserved bits set in fsverity_descriptor\n");
		return -EINVAL;
	}

	/* orig_file_size */
	vi->data_i_size = le64_to_cpu(desc->orig_file_size);
	if (vi->data_i_size <= 0) {
		pr_warn("Original file size is 0 or negative; this is unsupported\n");
		return -EINVAL;
	}

	/* extensions */
	desc_auth_len = parse_extensions(vi, desc, desc_len);
	if (desc_auth_len < 0)
		return desc_auth_len;

	if (!vi->have_root_hash) {
		pr_warn("Root hash wasn't found!\n");
		return -EINVAL;
	}

	/* Use an empty salt if no salt was found in the extensions list */
	if (!vi->hashstate) {
		err = set_salt(vi, "", 0);
		if (err)
			return err;
	}

	return desc_auth_len;
}

/*
 * Calculate the depth of the Merkle tree, then create a map from level to the
 * block offset at which that level's hash blocks start.  Level 'depth - 1' is
 * the root and is stored first.  Level 0 is the level directly "above" the data
 * blocks and is stored last, just before the fsverity_descriptor.
 */
static int compute_tree_depth_and_offsets(struct fsverity_info *vi)
{
	unsigned int hashes_per_block = 1 << vi->log_arity;
	u64 blocks = ((u64)vi->data_i_size + (1 << vi->block_bits) - 1) >>
			vi->block_bits;
	u64 offset = blocks;	/* assuming Merkle tree past EOF */
	int depth = 0;
	int i;

	while (blocks > 1) {
		if (depth >= FS_VERITY_MAX_LEVELS) {
			pr_warn("Too many tree levels (max is %d)\n",
				FS_VERITY_MAX_LEVELS);
			return -EINVAL;
		}
		blocks = (blocks + hashes_per_block - 1) >> vi->log_arity;
		vi->hash_lvl_region_idx[depth++] = blocks;
	}
	vi->depth = depth;

	for (i = depth - 1; i >= 0; i--) {
		u64 next_count = vi->hash_lvl_region_idx[i];

		vi->hash_lvl_region_idx[i] = offset;
		pr_debug("Level %d is [%llu..%llu] (%llu blocks)\n",
			 i, offset, offset + next_count - 1, next_count);
		offset += next_count;
	}
	return 0;
}

/* Arbitrary limit, can be increased if needed */
#define MAX_DESCRIPTOR_PAGES	16

/*
 * Compute the file's measurement by hashing the first 'desc_auth_len' bytes of
 * the fs-verity descriptor (which includes the Merkle tree root hash as an
 * authenticated extension item).
 *
 * Note: 'desc' may point into vmap'ed memory, so it can't be passed directly to
 * sg_set_buf() for the ahash API.  Instead, we pass the pages directly.
 */
static int compute_measurement(const struct fsverity_info *vi,
			       const struct fsverity_descriptor *desc,
			       int desc_auth_len,
			       struct page *desc_pages[MAX_DESCRIPTOR_PAGES],
			       int nr_desc_pages, u8 *measurement)
{
	struct ahash_request *req;
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist sg[MAX_DESCRIPTOR_PAGES];
	int offset, len, remaining;
	int i;
	int err;

	req = ahash_request_alloc(vi->hash_alg->tfm, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	sg_init_table(sg, nr_desc_pages);
	offset = offset_in_page(desc);
	remaining = desc_auth_len;
	for (i = 0; i < nr_desc_pages && remaining; i++) {
		len = min_t(int, PAGE_SIZE - offset, remaining);
		sg_set_page(&sg[i], desc_pages[i], len, offset);
		remaining -= len;
		offset = 0;
	}

	ahash_request_set_callback(req, CRYPTO_TFM_REQ_MAY_SLEEP |
				   CRYPTO_TFM_REQ_MAY_BACKLOG,
				   crypto_req_done, &wait);
	ahash_request_set_crypt(req, sg, measurement, desc_auth_len);
	err = crypto_wait_req(crypto_ahash_digest(req), &wait);
	ahash_request_free(req);
	return err;
}

/*
 * Compute the file's measurement; then, if a signature was present, verify that
 * the signed measurement matches the actual one.
 */
static int
verify_file_measurement(struct fsverity_info *vi,
			const struct fsverity_descriptor *desc,
			int desc_auth_len,
			struct page *desc_pages[MAX_DESCRIPTOR_PAGES],
			int nr_desc_pages)
{
	u8 measurement[FS_VERITY_MAX_DIGEST_SIZE];
	int err;

	err = compute_measurement(vi, desc, desc_auth_len, desc_pages,
				  nr_desc_pages, measurement);
	if (err) {
		pr_warn("Error computing fs-verity measurement: %d\n", err);
		return err;
	}

	if (!vi->have_signed_measurement) {
		pr_debug("Computed measurement: %s:%*phN (used desc_auth_len %d)\n",
			 vi->hash_alg->name, vi->hash_alg->digest_size,
			 measurement, desc_auth_len);
		if (fsverity_require_signatures) {
			pr_warn("require_signatures=1, rejecting unsigned file!\n");
			return -EBADMSG;
		}
		memcpy(vi->measurement, measurement, vi->hash_alg->digest_size);
		return 0;
	}

	if (!memcmp(measurement, vi->measurement, vi->hash_alg->digest_size)) {
		pr_debug("Verified measurement: %s:%*phN (used desc_auth_len %d)\n",
			 vi->hash_alg->name, vi->hash_alg->digest_size,
			 measurement, desc_auth_len);
		return 0;
	}

	pr_warn("FILE CORRUPTED (actual measurement mismatches signed measurement): "
		"want %s:%*phN, real %s:%*phN (used desc_auth_len %d)\n",
		vi->hash_alg->name, vi->hash_alg->digest_size, vi->measurement,
		vi->hash_alg->name, vi->hash_alg->digest_size, measurement,
		desc_auth_len);
	return -EBADMSG;
}

static struct fsverity_info *alloc_fsverity_info(void)
{
	return kmem_cache_zalloc(fsverity_info_cachep, GFP_NOFS);
}

void free_fsverity_info(struct fsverity_info *vi)
{
	if (!vi)
		return;
	kfree(vi->hashstate);
	kmem_cache_free(fsverity_info_cachep, vi);
}

/**
 * find_fsverity_footer - find the fsverity_footer in the last page of metadata
 *
 * Allow the fs-verity footer to be padded with zeroes.  This is needed by ext4,
 * which stores the fs-verity metadata beyond EOF but sets i_size = data_i_size.
 * Then, the fs-verity footer must be found implicitly via the last extent.
 *
 * Return: pointer to the footer if found, else NULL
 */
static const struct fsverity_footer *
find_fsverity_footer(const u8 *last_virt, size_t last_validsize)
{
	const u8 *p = last_virt + last_validsize;
	const struct fsverity_footer *ftr;

	/* Find the last nonzero byte, which should be ftr->magic[7] */
	do {
		if (p <= last_virt)
			return NULL;
	} while (*--p == 0);

	BUILD_BUG_ON(sizeof(ftr->magic) != 8);
	BUILD_BUG_ON(offsetof(struct fsverity_footer, magic[8]) !=
		     sizeof(*ftr));
	if (p - last_virt < offsetof(struct fsverity_footer, magic[7]))
		return NULL;
	ftr = container_of(p, struct fsverity_footer, magic[7]);
	if (memcmp(ftr->magic, FS_VERITY_MAGIC, sizeof(ftr->magic)))
		return NULL;
	return ftr;
}

struct page *fsverity_read_metadata_page(struct inode *inode, pgoff_t index)
{
	/*
	 * For now we assume that the verity metadata is stored in the same data
	 * stream as the actual file contents (as ext4 and f2fs do), so we read
	 * the metadata directly from the inode's page cache.  If any
	 * filesystems need to do things differently, this should be replaced
	 * with a method fsverity_operations.read_metadata_page().
	 */
	return read_mapping_page(inode->i_mapping, index, NULL);
}

/**
 * map_fsverity_descriptor - map an inode's fs-verity descriptor into memory
 *
 * If the descriptor fits in one page, we use kmap; otherwise we use vmap.
 * unmap_fsverity_descriptor() must be called later to unmap it.
 *
 * It's assumed that the file contents cannot be modified concurrently.
 * (This is guaranteed by either deny_write_access() or by the verity bit.)
 *
 * Return: the virtual address of the start of the descriptor, in virtually
 * contiguous memory.  Also fills in desc_pages and returns in *desc_len the
 * length of the descriptor including all extensions, and in *desc_start the
 * offset of the descriptor from the start of the file, in bytes.
 */
static const struct fsverity_descriptor *
map_fsverity_descriptor(struct inode *inode, loff_t metadata_end,
			struct page *desc_pages[MAX_DESCRIPTOR_PAGES],
			int *nr_desc_pages, int *desc_len, loff_t *desc_start)
{
	const int last_validsize = ((metadata_end - 1) & ~PAGE_MASK) + 1;
	const pgoff_t last_pgoff = (metadata_end - 1) >> PAGE_SHIFT;
	struct page *last_page;
	const void *last_virt;
	const struct fsverity_footer *ftr;
	pgoff_t first_pgoff;
	u32 desc_reverse_offset;
	pgoff_t pgoff;
	const void *desc_virt;
	int i;
	int err;

	*nr_desc_pages = 0;
	*desc_len = 0;
	*desc_start = 0;

	last_page = fsverity_read_metadata_page(inode, last_pgoff);
	if (IS_ERR(last_page)) {
		pr_warn("Error reading last page: %ld\n", PTR_ERR(last_page));
		return ERR_CAST(last_page);
	}
	last_virt = kmap(last_page);

	ftr = find_fsverity_footer(last_virt, last_validsize);
	if (!ftr) {
		pr_warn("No verity metadata found\n");
		err = -EINVAL;
		goto err_out;
	}
	metadata_end -= (last_virt + last_validsize - sizeof(*ftr)) -
			(void *)ftr;

	desc_reverse_offset = le32_to_cpu(ftr->desc_reverse_offset);
	if (desc_reverse_offset <
	    sizeof(struct fsverity_descriptor) + sizeof(*ftr) ||
	    desc_reverse_offset > metadata_end) {
		pr_warn("Unexpected desc_reverse_offset: %u\n",
			desc_reverse_offset);
		err = -EINVAL;
		goto err_out;
	}
	*desc_start = metadata_end - desc_reverse_offset;
	if (*desc_start & 7) {
		pr_warn("fs-verity descriptor is misaligned (desc_start=%lld)\n",
			*desc_start);
		err = -EINVAL;
		goto err_out;
	}

	first_pgoff = *desc_start >> PAGE_SHIFT;
	if (last_pgoff - first_pgoff >= MAX_DESCRIPTOR_PAGES) {
		pr_warn("fs-verity descriptor is too long (%lu pages)\n",
			last_pgoff - first_pgoff + 1);
		err = -EINVAL;
		goto err_out;
	}

	*desc_len = desc_reverse_offset - sizeof(__le32);

	if (first_pgoff == last_pgoff) {
		/* Single-page descriptor; use the already-kmapped last page */
		desc_pages[0] = last_page;
		*nr_desc_pages = 1;
		return last_virt + (*desc_start & ~PAGE_MASK);
	}

	/* Multi-page descriptor; map the additional pages into memory */

	for (pgoff = first_pgoff; pgoff < last_pgoff; pgoff++) {
		struct page *page;

		page = fsverity_read_metadata_page(inode, pgoff);
		if (IS_ERR(page)) {
			err = PTR_ERR(page);
			pr_warn("Error reading descriptor page: %d\n", err);
			goto err_out;
		}
		desc_pages[(*nr_desc_pages)++] = page;
	}

	desc_pages[(*nr_desc_pages)++] = last_page;
	kunmap(last_page);
	last_page = NULL;

	desc_virt = vmap(desc_pages, *nr_desc_pages, VM_MAP, PAGE_KERNEL_RO);
	if (!desc_virt) {
		err = -ENOMEM;
		goto err_out;
	}

	return desc_virt + (*desc_start & ~PAGE_MASK);

err_out:
	for (i = 0; i < *nr_desc_pages; i++)
		put_page(desc_pages[i]);
	if (last_page) {
		kunmap(last_page);
		put_page(last_page);
	}
	return ERR_PTR(err);
}

static void
unmap_fsverity_descriptor(const struct fsverity_descriptor *desc,
			  struct page *desc_pages[MAX_DESCRIPTOR_PAGES],
			  int nr_desc_pages)
{
	int i;

	if (is_vmalloc_addr(desc)) {
		vunmap((void *)((unsigned long)desc & PAGE_MASK));
	} else {
		WARN_ON(nr_desc_pages != 1);
		kunmap(desc_pages[0]);
	}
	for (i = 0; i < nr_desc_pages; i++)
		put_page(desc_pages[i]);
}

/* Read the file's fs-verity descriptor and create an fsverity_info for it */
struct fsverity_info *create_fsverity_info(struct inode *inode, bool enabling)
{
	struct fsverity_info *vi;
	const struct fsverity_descriptor *desc = NULL;
	struct page *desc_pages[MAX_DESCRIPTOR_PAGES];
	int nr_desc_pages;
	int desc_len;
	loff_t desc_start;
	int desc_auth_len;
	int err;

	vi = alloc_fsverity_info();
	if (!vi)
		return ERR_PTR(-ENOMEM);

	if (enabling) {
		/* file is in fsveritysetup format */
		vi->metadata_end = i_size_read(inode);
	} else {
		/* verity metadata may be in a filesystem-specific location */
		err = inode->i_sb->s_vop->get_metadata_end(inode,
							   &vi->metadata_end);
		if (err)
			goto out;
	}

	desc = map_fsverity_descriptor(inode, vi->metadata_end, desc_pages,
				       &nr_desc_pages, &desc_len, &desc_start);
	if (IS_ERR(desc)) {
		err = PTR_ERR(desc);
		desc = NULL;
		goto out;
	}

	dump_fsverity_descriptor(desc);
	desc_auth_len = parse_fsverity_descriptor(vi, desc, desc_len);
	if (desc_auth_len < 0) {
		err = desc_auth_len;
		goto out;
	}
	if (vi->data_i_size > i_size_read(inode)) {
		pr_warn("Bad data_i_size: %llu\n", vi->data_i_size);
		err = -EINVAL;
		goto out;
	}

	err = compute_tree_depth_and_offsets(vi);
	if (err)
		goto out;
	err = verify_file_measurement(vi, desc, desc_auth_len,
				      desc_pages, nr_desc_pages);
out:
	if (desc)
		unmap_fsverity_descriptor(desc, desc_pages, nr_desc_pages);
	if (err) {
		free_fsverity_info(vi);
		vi = ERR_PTR(err);
	}
	return vi;
}

/* Ensure the inode has an ->i_verity_info */
static int setup_fsverity_info(struct inode *inode)
{
	struct fsverity_info *vi = get_fsverity_info(inode);

	if (vi)
		return 0;

	vi = create_fsverity_info(inode, false);
	if (IS_ERR(vi))
		return PTR_ERR(vi);

	if (!set_fsverity_info(inode, vi))
		free_fsverity_info(vi);
	return 0;
}

/**
 * fsverity_file_open - prepare to open a verity file
 * @inode: the inode being opened
 * @filp: the struct file being set up
 *
 * When opening a verity file, deny the open if it is for writing.  Otherwise,
 * set up the inode's ->i_verity_info (if not already done) by parsing the
 * verity metadata at the end of the file.
 *
 * When combined with fscrypt, this must be called after fscrypt_file_open().
 * Otherwise, we won't have the key set up to decrypt the verity metadata.
 *
 * Return: 0 on success, -errno on failure
 */
int fsverity_file_open(struct inode *inode, struct file *filp)
{
	if (filp->f_mode & FMODE_WRITE) {
		pr_debug("Denying opening verity file (ino %lu) for write\n",
			 inode->i_ino);
		return -EPERM;
	}

	return setup_fsverity_info(inode);
}
EXPORT_SYMBOL_GPL(fsverity_file_open);

/**
 * fsverity_prepare_setattr - prepare to change a verity inode's attributes
 * @dentry: dentry through which the inode is being changed
 * @attr: attributes to change
 *
 * Verity files are immutable, so deny truncates.  This isn't covered by the
 * open-time check because sys_truncate() takes a path, not a file descriptor.
 *
 * Return: 0 on success, -errno on failure
 */
int fsverity_prepare_setattr(struct dentry *dentry, struct iattr *attr)
{
	if (attr->ia_valid & ATTR_SIZE) {
		pr_debug("Denying truncate of verity file (ino %lu)\n",
			 d_inode(dentry)->i_ino);
		return -EPERM;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(fsverity_prepare_setattr);

/**
 * fsverity_prepare_getattr - prepare to get a verity inode's attributes
 * @inode: the inode for which the attributes are being retrieved
 *
 * This only needs to be called by filesystems that set the on-disk i_size of
 * verity files to something other than the data size, as then this is needed to
 * override i_size so that stat() shows the correct size.
 *
 * When the filesystem supports fscrypt too, it must make sure to set up the
 * inode's encryption key (if needed) before calling this.
 *
 * Return: 0 on success, -errno on failure
 */
int fsverity_prepare_getattr(struct inode *inode)
{
	return setup_fsverity_info(inode);
}
EXPORT_SYMBOL_GPL(fsverity_prepare_getattr);

/**
 * fsverity_cleanup_inode - free the inode's verity info, if present
 *
 * Filesystems must call this on inode eviction to free ->i_verity_info.
 */
void fsverity_cleanup_inode(struct inode *inode)
{
	free_fsverity_info(inode->i_verity_info);
	inode->i_verity_info = NULL;
}
EXPORT_SYMBOL_GPL(fsverity_cleanup_inode);

/**
 * fsverity_full_i_size - get the full file size
 *
 * If the file has fs-verity set up, return the full file size including the
 * verity metadata.  Otherwise just return i_size.  This is only meaningful when
 * the filesystem stores the verity metadata past EOF.
 */
loff_t fsverity_full_i_size(const struct inode *inode)
{
	struct fsverity_info *vi = get_fsverity_info(inode);

	if (vi)
		return vi->metadata_end;

	return i_size_read(inode);
}
EXPORT_SYMBOL_GPL(fsverity_full_i_size);

static int __init fsverity_module_init(void)
{
	int err;

	/*
	 * Use an unbound workqueue to allow bios to be verified in parallel
	 * even when they happen to complete on the same CPU.  This sacrifices
	 * locality, but it's worthwhile since hashing is CPU-intensive.
	 *
	 * Also use a high-priority workqueue to prioritize verification work,
	 * which blocks reads from completing, over regular application tasks.
	 */
	err = -ENOMEM;
	fsverity_read_workqueue = alloc_workqueue("fsverity_read_queue",
						  WQ_UNBOUND | WQ_HIGHPRI,
						  num_online_cpus());
	if (!fsverity_read_workqueue)
		goto error;

	err = -ENOMEM;
	fsverity_info_cachep = KMEM_CACHE_USERCOPY(fsverity_info,
						   SLAB_RECLAIM_ACCOUNT,
						   measurement);
	if (!fsverity_info_cachep)
		goto error_free_workqueue;

	err = fsverity_signature_init();
	if (err)
		goto error_free_info_cache;

	fsverity_check_hash_algs();

	pr_debug("Initialized fs-verity\n");
	return 0;

error_free_info_cache:
	kmem_cache_destroy(fsverity_info_cachep);
error_free_workqueue:
	destroy_workqueue(fsverity_read_workqueue);
error:
	return err;
}

static void __exit fsverity_module_exit(void)
{
	destroy_workqueue(fsverity_read_workqueue);
	kmem_cache_destroy(fsverity_info_cachep);
	fsverity_signature_exit();
	fsverity_exit_hash_algs();
}

module_init(fsverity_module_init)
module_exit(fsverity_module_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("fs-verity: read-only file-based authenticity protection");
