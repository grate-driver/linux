/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Network filesystem support services.
 *
 * Copyright (C) 2021 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * See:
 *
 *	Documentation/filesystems/netfs_library.rst
 *
 * for a description of the network filesystem interface declared here.
 */

#ifndef _LINUX_NETFS_H
#define _LINUX_NETFS_H

#include <linux/pagemap.h>

/*
 * Overload PG_private_2 to give us PG_fscache - this is used to indicate that
 * a page is currently backed by a local disk cache
 */
#define PageFsCache(page)		PagePrivate2((page))
#define SetPageFsCache(page)		SetPagePrivate2((page))
#define ClearPageFsCache(page)		ClearPagePrivate2((page))
#define TestSetPageFsCache(page)	TestSetPagePrivate2((page))
#define TestClearPageFsCache(page)	TestClearPagePrivate2((page))

/**
 * unlock_page_fscache - Unlock a page that's locked with PG_fscache
 * @page: The page
 *
 * Unlocks a page that's locked with PG_fscache and wakes up sleepers in
 * wait_on_page_fscache().  This page bit is used by the netfs helpers when a
 * netfs page is being written to a local disk cache, thereby allowing writes
 * to the cache for the same page to be serialised.
 */
static inline void unlock_page_fscache(struct page *page)
{
	unlock_page_private_2(page);
}

/**
 * wait_on_page_fscache - Wait for PG_fscache to be cleared on a page
 * @page: The page
 *
 * Wait for the PG_fscache (PG_private_2) page bit to be removed from a page.
 * This is, for example, used to handle a netfs page being written to a local
 * disk cache, thereby allowing writes to the cache for the same page to be
 * serialised.
 */
static inline void wait_on_page_fscache(struct page *page)
{
	if (PageFsCache(page))
		wait_on_page_bit(compound_head(page), PG_fscache);
}

#endif /* _LINUX_NETFS_H */
