// SPDX-License-Identifier: GPL-2.0
/*
 * fs/verity/hash_algs.c: fs-verity hash algorithm management
 *
 * Copyright 2018 Google LLC
 *
 * Written by Eric Biggers.
 */

#include "fsverity_private.h"

#include <crypto/hash.h>

/* The list of hash algorithms supported by fs-verity */
struct fsverity_hash_alg fsverity_hash_algs[] = {
	[FS_VERITY_ALG_SHA256] = {
		.name = "sha256",
		.digest_size = 32,
		.cryptographic = true,
	},
	[FS_VERITY_ALG_SHA512] = {
		.name = "sha512",
		.digest_size = 64,
		.cryptographic = true,
	},
	[FS_VERITY_ALG_CRC32C] = {
		.name = "crc32c",
		.digest_size = 4,
	},
};

/*
 * Translate the given fs-verity hash algorithm number into a struct describing
 * the algorithm, and ensure it has a hash transform ready to go.  The hash
 * transforms are allocated on-demand firstly to not waste resources when they
 * aren't needed, and secondly because the fs-verity module may be loaded
 * earlier than the needed crypto modules.
 */
const struct fsverity_hash_alg *fsverity_get_hash_alg(unsigned int num)
{
	struct fsverity_hash_alg *alg;
	struct crypto_ahash *tfm;
	int err;

	if (num >= ARRAY_SIZE(fsverity_hash_algs) ||
	    !fsverity_hash_algs[num].digest_size) {
		pr_warn("Unknown hash algorithm: %u\n", num);
		return ERR_PTR(-EINVAL);
	}
	alg = &fsverity_hash_algs[num];
retry:
	/* pairs with cmpxchg_release() below */
	tfm = smp_load_acquire(&alg->tfm);
	if (tfm)
		return alg;
	/*
	 * Using the shash API would make things a bit simpler, but the ahash
	 * API is preferable as it allows the use of crypto accelerators.
	 */
	tfm = crypto_alloc_ahash(alg->name, 0, 0);
	if (IS_ERR(tfm)) {
		if (PTR_ERR(tfm) == -ENOENT)
			pr_warn("Algorithm %u (%s) is unavailable\n",
				num, alg->name);
		else
			pr_warn("Error allocating algorithm %u (%s): %ld\n",
				num, alg->name, PTR_ERR(tfm));
		return ERR_CAST(tfm);
	}

	err = -EINVAL;
	if (WARN_ON(alg->digest_size != crypto_ahash_digestsize(tfm)))
		goto err_free_tfm;

	pr_info("%s using implementation \"%s\"\n", alg->name,
		crypto_hash_alg_common(tfm)->base.cra_driver_name);

	/* pairs with smp_load_acquire() above */
	if (cmpxchg_release(&alg->tfm, NULL, tfm) != NULL) {
		crypto_free_ahash(tfm);
		goto retry;
	}

	return alg;

err_free_tfm:
	crypto_free_ahash(tfm);
	return ERR_PTR(err);
}

void __init fsverity_check_hash_algs(void)
{
	int i;

	/*
	 * Sanity check the digest sizes (could be a build-time check, but
	 * they're in an array)
	 */
	for (i = 0; i < ARRAY_SIZE(fsverity_hash_algs); i++) {
		struct fsverity_hash_alg *alg = &fsverity_hash_algs[i];

		if (!alg->digest_size)
			continue;
		BUG_ON(alg->digest_size > FS_VERITY_MAX_DIGEST_SIZE);
		BUG_ON(!is_power_of_2(alg->digest_size));
	}
}

void __exit fsverity_exit_hash_algs(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fsverity_hash_algs); i++)
		crypto_free_ahash(fsverity_hash_algs[i].tfm);
}
