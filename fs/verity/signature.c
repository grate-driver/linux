// SPDX-License-Identifier: GPL-2.0
/*
 * fs/verity/signature.c: verification of builtin signatures
 *
 * Copyright 2018 Google LLC
 *
 * Written by Eric Biggers.
 */

#include "fsverity_private.h"

#include <linux/cred.h>
#include <linux/key.h>
#include <linux/verification.h>

/*
 * /proc/sys/fs/verity/require_signatures
 * If 1, all verity files must have a valid builtin signature.
 */
int fsverity_require_signatures;

/*
 * Keyring that contains the trusted X.509 certificates.
 *
 * Only root (kuid=0) can modify this.  Also, root may use
 * keyctl_restrict_keyring() to prevent any more additions.
 */
static struct key *fsverity_keyring;

static int extract_measurement(void *ctx, const void *data, size_t len,
			       size_t asn1hdrlen)
{
	struct fsverity_info *vi = ctx;
	const struct fsverity_digest_disk *d;
	const struct fsverity_hash_alg *hash_alg;

	if (len < sizeof(*d)) {
		pr_warn("Signed file measurement has unrecognized format\n");
		return -EBADMSG;
	}
	d = (const void *)data;

	hash_alg = fsverity_get_hash_alg(le16_to_cpu(d->digest_algorithm));
	if (IS_ERR(hash_alg))
		return PTR_ERR(hash_alg);

	if (le16_to_cpu(d->digest_size) != hash_alg->digest_size) {
		pr_warn("Wrong digest_size in signed measurement: wanted %u for algorithm %s, but got %u\n",
			hash_alg->digest_size, hash_alg->name,
			le16_to_cpu(d->digest_size));
		return -EBADMSG;
	}

	if (len < sizeof(*d) + hash_alg->digest_size) {
		pr_warn("Signed file measurement is truncated\n");
		return -EBADMSG;
	}

	if (hash_alg != vi->hash_alg) {
		pr_warn("Signed file measurement uses %s, but file uses %s\n",
			hash_alg->name, vi->hash_alg->name);
		return -EBADMSG;
	}

	memcpy(vi->measurement, d->digest, hash_alg->digest_size);
	vi->have_signed_measurement = true;
	return 0;
}

/**
 * fsverity_parse_pkcs7_signature_extension - verify the signed file measurement
 *
 * Verify a signed fsverity_measurement against the certificates in the
 * fs-verity keyring.  The signature is given as a PKCS#7 formatted message, and
 * the signed data is included in the message (not detached).
 *
 * Return: 0 if the signature checks out and the signed measurement is
 * well-formed and uses the expected hash algorithm; -EBADMSG on signature
 * verification failure or malformed data; else another -errno code.
 */
int fsverity_parse_pkcs7_signature_extension(struct fsverity_info *vi,
					     const void *raw_pkcs7, size_t size)
{
	int err;

	if (vi->have_signed_measurement) {
		pr_warn("Found multiple PKCS#7 signatures\n");
		return -EBADMSG;
	}

	if (!vi->hash_alg->cryptographic) {
		/* Might as well check this... */
		pr_warn("Found signed %s file measurement, but %s isn't a cryptographic hash algorithm.\n",
			vi->hash_alg->name, vi->hash_alg->name);
		return -EBADMSG;
	}

	err = verify_pkcs7_signature(NULL, 0, raw_pkcs7, size, fsverity_keyring,
				     VERIFYING_UNSPECIFIED_SIGNATURE,
				     extract_measurement, vi);
	if (err)
		pr_warn("PKCS#7 signature verification error: %d\n", err);

	return err;
}

#ifdef CONFIG_SYSCTL
static int zero;
static int one = 1;
static struct ctl_table_header *fsverity_sysctl_header;

static const struct ctl_path fsverity_sysctl_path[] = {
	{ .procname = "fs", },
	{ .procname = "verity", },
	{ }
};

static struct ctl_table fsverity_sysctl_table[] = {
	{
		.procname       = "require_signatures",
		.data           = &fsverity_require_signatures,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec_minmax,
		.extra1         = &zero,
		.extra2         = &one,
	},
	{ }
};

static int __init fsverity_sysctl_init(void)
{
	fsverity_sysctl_header = register_sysctl_paths(fsverity_sysctl_path,
						       fsverity_sysctl_table);
	if (!fsverity_sysctl_header) {
		pr_warn("sysctl registration failed!");
		return -ENOMEM;
	}
	return 0;
}

static void __exit fsverity_sysctl_exit(void)
{
	unregister_sysctl_table(fsverity_sysctl_header);
}
#else /* CONFIG_SYSCTL */
static inline int fsverity_sysctl_init(void)
{
	return 0;
}

static inline void fsverity_sysctl_exit(void)
{
}
#endif /* !CONFIG_SYSCTL */

int __init fsverity_signature_init(void)
{
	struct key *ring;
	int err;

	ring = keyring_alloc(".fs-verity", KUIDT_INIT(0), KGIDT_INIT(0),
			     current_cred(),
			     ((KEY_POS_ALL & ~KEY_POS_SETATTR) |
			      KEY_USR_VIEW | KEY_USR_READ |
			      KEY_USR_WRITE | KEY_USR_SEARCH | KEY_USR_SETATTR),
			     KEY_ALLOC_NOT_IN_QUOTA, NULL, NULL);
	if (IS_ERR(ring))
		return PTR_ERR(ring);

	err = fsverity_sysctl_init();
	if (err)
		goto error_put_ring;

	fsverity_keyring = ring;
	return 0;

error_put_ring:
	key_put(ring);
	return err;
}

void __exit fsverity_signature_exit(void)
{
	key_put(fsverity_keyring);
	fsverity_sysctl_exit();
}
