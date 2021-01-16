/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_SUBPAGE_H
#define BTRFS_SUBPAGE_H

#include <linux/spinlock.h>
#include "ctree.h"

/*
 * Maximum page size we support is 64K, minimum sector size is 4K, u16 bitmap
 * is sufficient. Regular bitmap_* is not used due to size reasons.
 */
#define BTRFS_SUBPAGE_BITMAP_SIZE	16

/*
 * Structure to trace status of each sector inside a page, attached to
 * page::private for both data and metadata inodes.
 */
struct btrfs_subpage {
	/* Common members for both data and metadata pages */
	spinlock_t lock;
	union {
		/* Structures only used by metadata */
		bool under_alloc;
		/* Structures only used by data */
	};
};

/* Allocate additional data where page represents more than one sector */
static inline int btrfs_alloc_subpage(struct btrfs_fs_info *fs_info,
				      struct btrfs_subpage **ret)
{
	if (fs_info->sectorsize == PAGE_SIZE)
		return 0;

	*ret = kzalloc(sizeof(struct btrfs_subpage), GFP_NOFS);
	if (!*ret)
		return -ENOMEM;
	return 0;
}

/* Prevent freeing page private when page metadata are allocated */
static inline void btrfs_page_start_meta_alloc(struct btrfs_fs_info *fs_info,
					       struct page *page)
{
	struct btrfs_subpage *subpage;

	if (fs_info->sectorsize == PAGE_SIZE)
		return;

	ASSERT(PagePrivate(page) && page->mapping);

	subpage = (struct btrfs_subpage *)page->private;
	subpage->under_alloc = true;
}

static inline void btrfs_page_end_meta_alloc(struct btrfs_fs_info *fs_info,
					     struct page *page)
{
	struct btrfs_subpage *subpage;

	if (fs_info->sectorsize == PAGE_SIZE)
		return;

	ASSERT(PagePrivate(page) && page->mapping);

	subpage = (struct btrfs_subpage *)page->private;
	subpage->under_alloc = false;
}

int btrfs_attach_subpage(struct btrfs_fs_info *fs_info, struct page *page);
void btrfs_detach_subpage(struct btrfs_fs_info *fs_info, struct page *page);

#endif
