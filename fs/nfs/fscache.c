// SPDX-License-Identifier: GPL-2.0-or-later
/* NFS filesystem cache interface
 *
 * Copyright (C) 2008 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_fs_sb.h>
#include <linux/in6.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/iversion.h>
#include <linux/xarray.h>
#define FSCACHE_USE_NEW_IO_API
#include <linux/fscache.h>
#include <linux/netfs.h>

#include "internal.h"
#include "iostat.h"
#include "fscache.h"

#define NFSDBG_FACILITY		NFSDBG_FSCACHE

static struct rb_root nfs_fscache_keys = RB_ROOT;
static DEFINE_SPINLOCK(nfs_fscache_keys_lock);

/*
 * Layout of the key for an NFS server cache object.
 */
struct nfs_server_key {
	struct {
		uint16_t	nfsversion;		/* NFS protocol version */
		uint32_t	minorversion;		/* NFSv4 minor version */
		uint16_t	family;			/* address family */
		__be16		port;			/* IP port */
	} hdr;
	union {
		struct in_addr	ipv4_addr;	/* IPv4 address */
		struct in6_addr ipv6_addr;	/* IPv6 address */
	};
} __packed;

/*
 * Get the per-client index cookie for an NFS client if the appropriate mount
 * flag was set
 * - We always try and get an index cookie for the client, but get filehandle
 *   cookies on a per-superblock basis, depending on the mount flags
 */
void nfs_fscache_get_client_cookie(struct nfs_client *clp)
{
	const struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) &clp->cl_addr;
	const struct sockaddr_in *sin = (struct sockaddr_in *) &clp->cl_addr;
	struct nfs_server_key key;
	uint16_t len = sizeof(key.hdr);

	memset(&key, 0, sizeof(key));
	key.hdr.nfsversion = clp->rpc_ops->version;
	key.hdr.minorversion = clp->cl_minorversion;
	key.hdr.family = clp->cl_addr.ss_family;

	switch (clp->cl_addr.ss_family) {
	case AF_INET:
		key.hdr.port = sin->sin_port;
		key.ipv4_addr = sin->sin_addr;
		len += sizeof(key.ipv4_addr);
		break;

	case AF_INET6:
		key.hdr.port = sin6->sin6_port;
		key.ipv6_addr = sin6->sin6_addr;
		len += sizeof(key.ipv6_addr);
		break;

	default:
		printk(KERN_WARNING "NFS: Unknown network family '%d'\n",
		       clp->cl_addr.ss_family);
		clp->fscache = NULL;
		return;
	}

	/* create a cache index for looking up filehandles */
	clp->fscache = fscache_acquire_cookie(nfs_fscache_netfs.primary_index,
					      &nfs_fscache_server_index_def,
					      &key, len,
					      NULL, 0,
					      clp, 0, true);
	dfprintk(FSCACHE, "NFS: get client cookie (0x%p/0x%p)\n",
		 clp, clp->fscache);
}

/*
 * Dispose of a per-client cookie
 */
void nfs_fscache_release_client_cookie(struct nfs_client *clp)
{
	dfprintk(FSCACHE, "NFS: releasing client cookie (0x%p/0x%p)\n",
		 clp, clp->fscache);

	fscache_relinquish_cookie(clp->fscache, NULL, false);
	clp->fscache = NULL;
}

/*
 * Get the cache cookie for an NFS superblock.  We have to handle
 * uniquification here because the cache doesn't do it for us.
 *
 * The default uniquifier is just an empty string, but it may be overridden
 * either by the 'fsc=xxx' option to mount, or by inheriting it from the parent
 * superblock across an automount point of some nature.
 */
void nfs_fscache_get_super_cookie(struct super_block *sb, const char *uniq, int ulen)
{
	struct nfs_fscache_key *key, *xkey;
	struct nfs_server *nfss = NFS_SB(sb);
	struct rb_node **p, *parent;
	int diff;

	nfss->fscache_key = NULL;
	nfss->fscache = NULL;
	if (!uniq) {
		uniq = "";
		ulen = 1;
	}

	key = kzalloc(sizeof(*key) + ulen, GFP_KERNEL);
	if (!key)
		return;

	key->nfs_client = nfss->nfs_client;
	key->key.super.s_flags = sb->s_flags & NFS_SB_MASK;
	key->key.nfs_server.flags = nfss->flags;
	key->key.nfs_server.rsize = nfss->rsize;
	key->key.nfs_server.wsize = nfss->wsize;
	key->key.nfs_server.acregmin = nfss->acregmin;
	key->key.nfs_server.acregmax = nfss->acregmax;
	key->key.nfs_server.acdirmin = nfss->acdirmin;
	key->key.nfs_server.acdirmax = nfss->acdirmax;
	key->key.nfs_server.fsid = nfss->fsid;
	key->key.rpc_auth.au_flavor = nfss->client->cl_auth->au_flavor;

	key->key.uniq_len = ulen;
	memcpy(key->key.uniquifier, uniq, ulen);

	spin_lock(&nfs_fscache_keys_lock);
	p = &nfs_fscache_keys.rb_node;
	parent = NULL;
	while (*p) {
		parent = *p;
		xkey = rb_entry(parent, struct nfs_fscache_key, node);

		if (key->nfs_client < xkey->nfs_client)
			goto go_left;
		if (key->nfs_client > xkey->nfs_client)
			goto go_right;

		diff = memcmp(&key->key, &xkey->key, sizeof(key->key));
		if (diff < 0)
			goto go_left;
		if (diff > 0)
			goto go_right;

		if (key->key.uniq_len == 0)
			goto non_unique;
		diff = memcmp(key->key.uniquifier,
			      xkey->key.uniquifier,
			      key->key.uniq_len);
		if (diff < 0)
			goto go_left;
		if (diff > 0)
			goto go_right;
		goto non_unique;

	go_left:
		p = &(*p)->rb_left;
		continue;
	go_right:
		p = &(*p)->rb_right;
	}

	rb_link_node(&key->node, parent, p);
	rb_insert_color(&key->node, &nfs_fscache_keys);
	spin_unlock(&nfs_fscache_keys_lock);
	nfss->fscache_key = key;

	/* create a cache index for looking up filehandles */
	nfss->fscache = fscache_acquire_cookie(nfss->nfs_client->fscache,
					       &nfs_fscache_super_index_def,
					       &key->key,
					       sizeof(key->key) + ulen,
					       NULL, 0,
					       nfss, 0, true);
	dfprintk(FSCACHE, "NFS: get superblock cookie (0x%p/0x%p)\n",
		 nfss, nfss->fscache);
	return;

non_unique:
	spin_unlock(&nfs_fscache_keys_lock);
	kfree(key);
	nfss->fscache_key = NULL;
	nfss->fscache = NULL;
	printk(KERN_WARNING "NFS:"
	       " Cache request denied due to non-unique superblock keys\n");
}

/*
 * release a per-superblock cookie
 */
void nfs_fscache_release_super_cookie(struct super_block *sb)
{
	struct nfs_server *nfss = NFS_SB(sb);

	dfprintk(FSCACHE, "NFS: releasing superblock cookie (0x%p/0x%p)\n",
		 nfss, nfss->fscache);

	fscache_relinquish_cookie(nfss->fscache, NULL, false);
	nfss->fscache = NULL;

	if (nfss->fscache_key) {
		spin_lock(&nfs_fscache_keys_lock);
		rb_erase(&nfss->fscache_key->node, &nfs_fscache_keys);
		spin_unlock(&nfs_fscache_keys_lock);
		kfree(nfss->fscache_key);
		nfss->fscache_key = NULL;
	}
}

static void nfs_fscache_update_auxdata(struct nfs_fscache_inode_auxdata *auxdata,
				  struct nfs_inode *nfsi)
{
	memset(auxdata, 0, sizeof(*auxdata));
	auxdata->mtime_sec  = nfsi->vfs_inode.i_mtime.tv_sec;
	auxdata->mtime_nsec = nfsi->vfs_inode.i_mtime.tv_nsec;
	auxdata->ctime_sec  = nfsi->vfs_inode.i_ctime.tv_sec;
	auxdata->ctime_nsec = nfsi->vfs_inode.i_ctime.tv_nsec;

	if (NFS_SERVER(&nfsi->vfs_inode)->nfs_client->rpc_ops->version == 4)
		auxdata->change_attr = inode_peek_iversion_raw(&nfsi->vfs_inode);
}

/*
 * Initialise the per-inode cache cookie pointer for an NFS inode.
 */
void nfs_fscache_init_inode(struct inode *inode)
{
	struct nfs_fscache_inode_auxdata auxdata;
	struct nfs_server *nfss = NFS_SERVER(inode);
	struct nfs_inode *nfsi = NFS_I(inode);

	nfsi->fscache = NULL;
	if (!(nfss->fscache && S_ISREG(inode->i_mode)))
		return;

	nfs_fscache_update_auxdata(&auxdata, nfsi);

	nfsi->fscache = fscache_acquire_cookie(NFS_SB(inode->i_sb)->fscache,
					       &nfs_fscache_inode_object_def,
					       nfsi->fh.data, nfsi->fh.size,
					       &auxdata, sizeof(auxdata),
					       nfsi, nfsi->vfs_inode.i_size, false);
}

/*
 * Release a per-inode cookie.
 */
void nfs_fscache_clear_inode(struct inode *inode)
{
	struct nfs_fscache_inode_auxdata auxdata;
	struct nfs_inode *nfsi = NFS_I(inode);
	struct fscache_cookie *cookie = nfs_i_fscache(inode);

	dfprintk(FSCACHE, "NFS: clear cookie (0x%p/0x%p)\n", nfsi, cookie);

	nfs_fscache_update_auxdata(&auxdata, nfsi);
	fscache_relinquish_cookie(cookie, &auxdata, false);
	nfsi->fscache = NULL;
}

static bool nfs_fscache_can_enable(void *data)
{
	struct inode *inode = data;

	return !inode_is_open_for_write(inode);
}

/*
 * Enable or disable caching for a file that is being opened as appropriate.
 * The cookie is allocated when the inode is initialised, but is not enabled at
 * that time.  Enablement is deferred to file-open time to avoid stat() and
 * access() thrashing the cache.
 *
 * For now, with NFS, only regular files that are open read-only will be able
 * to use the cache.
 *
 * We enable the cache for an inode if we open it read-only and it isn't
 * currently open for writing.  We disable the cache if the inode is open
 * write-only.
 *
 * The caller uses the file struct to pin i_writecount on the inode before
 * calling us when a file is opened for writing, so we can make use of that.
 *
 * Note that this may be invoked multiple times in parallel by parallel
 * nfs_open() functions.
 */
void nfs_fscache_open_file(struct inode *inode, struct file *filp)
{
	struct nfs_fscache_inode_auxdata auxdata;
	struct nfs_inode *nfsi = NFS_I(inode);
	struct fscache_cookie *cookie = nfs_i_fscache(inode);

	if (!fscache_cookie_valid(cookie))
		return;

	nfs_fscache_update_auxdata(&auxdata, nfsi);

	if (inode_is_open_for_write(inode)) {
		dfprintk(FSCACHE, "NFS: nfsi 0x%p disabling cache\n", nfsi);
		clear_bit(NFS_INO_FSCACHE, &nfsi->flags);
		fscache_disable_cookie(cookie, &auxdata, true);
	} else {
		dfprintk(FSCACHE, "NFS: nfsi 0x%p enabling cache\n", nfsi);
		fscache_enable_cookie(cookie, &auxdata, nfsi->vfs_inode.i_size,
				      nfs_fscache_can_enable, inode);
		if (fscache_cookie_enabled(cookie))
			set_bit(NFS_INO_FSCACHE, &NFS_I(inode)->flags);
	}
}
EXPORT_SYMBOL_GPL(nfs_fscache_open_file);

static void nfs_issue_op(struct netfs_read_subrequest *subreq)
{
	struct inode *inode = subreq->rreq->inode;
	struct nfs_readdesc *desc = subreq->rreq->netfs_priv;
	struct page *page;
	pgoff_t start = (subreq->start + subreq->transferred) >> PAGE_SHIFT;
	pgoff_t last = ((subreq->start + subreq->len -
			 subreq->transferred - 1) >> PAGE_SHIFT);
	XA_STATE(xas, &subreq->rreq->mapping->i_pages, start);

	dfprintk(FSCACHE, "NFS: %s(fsc:%p s:%lu l:%lu) subreq->start: %lld "
		 "subreq->len: %ld subreq->transferred: %ld\n",
		 __func__, nfs_i_fscache(inode), start, last, subreq->start,
		 subreq->len, subreq->transferred);

	nfs_add_fscache_stats(inode, NFSIOS_FSCACHE_PAGES_READ_FAIL,
			      last - start + 1);
	nfs_pageio_init_read(&desc->pgio, inode, false,
			     &nfs_async_read_completion_ops);

	desc->pgio.pg_fsc = subreq; /* used in completion */

	rcu_read_lock();
	xas_for_each(&xas, page, last) {
		subreq->error = readpage_async_filler(desc, page);
		if (subreq->error < 0)
			break;
	}
	rcu_read_unlock();
	nfs_pageio_complete_read(&desc->pgio, inode);
}

static bool nfs_clamp_length(struct netfs_read_subrequest *subreq)
{
	struct inode *inode = subreq->rreq->mapping->host;
	unsigned int rsize = NFS_SB(inode->i_sb)->rsize;

	if (subreq->len > rsize) {
		dfprintk(FSCACHE,
			 "NFS: %s(fsc:%p slen:%lu rsize: %u)\n",
			 __func__, nfs_i_fscache(inode), subreq->len, rsize);
		subreq->len = rsize;
	}

	return true;
}

static void nfs_cleanup(struct address_space *mapping, void *netfs_priv)
{
	; /* fscache assumes if netfs_priv is given we have cleanup */
}

atomic_t nfs_fscache_debug_id;
static void nfs_init_rreq(struct netfs_read_request *rreq, struct file *file)
{
	struct nfs_inode *nfsi = NFS_I(rreq->inode);

	if (nfsi->fscache && test_bit(NFS_INO_FSCACHE, &nfsi->flags))
		rreq->cookie_debug_id = atomic_inc_return(&nfs_fscache_debug_id);
}

static bool nfs_is_cache_enabled(struct inode *inode)
{
	struct nfs_inode *nfsi = NFS_I(inode);

	return nfsi->fscache && test_bit(NFS_INO_FSCACHE, &nfsi->flags);
}

static int nfs_begin_cache_operation(struct netfs_read_request *rreq)
{
	struct fscache_cookie *cookie = NFS_I(rreq->inode)->fscache;

	return fscache_begin_read_operation(rreq, cookie);
}

static struct netfs_read_request_ops nfs_fscache_req_ops = {
	.init_rreq		= nfs_init_rreq,
	.is_cache_enabled	= nfs_is_cache_enabled,
	.begin_cache_operation	= nfs_begin_cache_operation,
	.issue_op		= nfs_issue_op,
	.clamp_length		= nfs_clamp_length,
	.cleanup		= nfs_cleanup
};

/*
 * Retrieve a page from fscache
 */
int nfs_readpage_from_fscache(struct file *file,
			      struct page *page,
			      struct nfs_readdesc *desc)
{
	int ret;
	struct inode *inode = file_inode(file);

	if (!NFS_I(file_inode(file))->fscache)
		return -ENOBUFS;

	dfprintk(FSCACHE,
		 "NFS: readpage_from_fscache(fsc:%p/p:%p(i:%lx f:%lx)/0x%p)\n",
		 nfs_i_fscache(inode), page, page->index, page->flags, inode);

	ret = netfs_readpage(file, page, &nfs_fscache_req_ops, desc);

	switch (ret) {
	case 0: /* read submitted */
		dfprintk(FSCACHE, "NFS:    readpage_from_fscache: submitted\n");
		nfs_inc_fscache_stats(inode, NFSIOS_FSCACHE_PAGES_READ_OK);
		return ret;

	case -ENOBUFS: /* inode not in cache */
	case -ENODATA: /* page not in cache */
		nfs_inc_fscache_stats(inode, NFSIOS_FSCACHE_PAGES_READ_FAIL);
		dfprintk(FSCACHE, "NFS:    readpage_from_fscache %d\n", ret);
		return 1;

	default:
		dfprintk(FSCACHE, "NFS:    readpage_from_fscache %d\n", ret);
		nfs_inc_fscache_stats(inode, NFSIOS_FSCACHE_PAGES_READ_FAIL);
	}

	return ret;
}

/*
 * Retrieve a set of pages from fscache
 */
int nfs_readahead_from_fscache(struct nfs_readdesc *desc,
				 struct readahead_control *ractl)
{
	if (!NFS_I(ractl->mapping->host)->fscache)
		return -ENOBUFS;

	netfs_readahead(ractl, &nfs_fscache_req_ops, desc);

	/* FIXME: NFSIOS_NFSIOS_FSCACHE_ stats */
	return 0;
}

/*
 * Store a newly fetched data in fscache
 */
void nfs_read_completion_to_fscache(struct nfs_pgio_header *hdr,
				    unsigned long bytes)
{
	struct netfs_read_subrequest *subreq = hdr->fsc;

	if (NFS_I(hdr->inode)->fscache && subreq) {
		dfprintk(FSCACHE,
			 "NFS: read_completion_to_fscache(fsc:%p err:%d bytes:%lu subreq->len:%lu\n",
			 NFS_I(hdr->inode)->fscache, hdr->error, bytes, subreq->len);
		__set_bit(NETFS_SREQ_CLEAR_TAIL, &subreq->flags);
		netfs_subreq_terminated(subreq, hdr->error ?: bytes);
		hdr->fsc = NULL;
	}
}
