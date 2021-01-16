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
	u16 uptodate_bitmap;
	u16 error_bitmap;
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

/*
 * Convert the [start, start + len) range into a u16 bitmap
 *
 * For example: if start == page_offset() + 16K, len = 16K, we get 0x00f0.
 */
static inline u16 btrfs_subpage_calc_bitmap(const struct btrfs_fs_info *fs_info,
			struct page *page, u64 start, u32 len)
{
	const int bit_start = offset_in_page(start) >> fs_info->sectorsize_bits;
	const int nbits = len >> fs_info->sectorsize_bits;

	/* Basic checks */
	ASSERT(PagePrivate(page) && page->private);
	ASSERT(IS_ALIGNED(start, fs_info->sectorsize) &&
	       IS_ALIGNED(len, fs_info->sectorsize));

	/*
	 * The range check only works for mapped page, we can still have
	 * unampped page like dummy extent buffer pages.
	 */
	if (page->mapping)
		ASSERT(page_offset(page) <= start &&
			start + len <= page_offset(page) + PAGE_SIZE);
	/*
	 * Here nbits can be 16, thus can go beyond u16 range. We make the
	 * first left shift to be calculate in unsigned long (at least u32),
	 * then truncate the result to u16.
	 */
	return (u16)(((1UL << nbits) - 1) << bit_start);
}

static inline void btrfs_subpage_set_uptodate(const struct btrfs_fs_info *fs_info,
			struct page *page, u64 start, u32 len)
{
	struct btrfs_subpage *subpage = (struct btrfs_subpage *)page->private;
	const u16 tmp = btrfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->uptodate_bitmap |= tmp;
	if (subpage->uptodate_bitmap == U16_MAX)
		SetPageUptodate(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

static inline void btrfs_subpage_clear_uptodate(const struct btrfs_fs_info *fs_info,
			struct page *page, u64 start, u32 len)
{
	struct btrfs_subpage *subpage = (struct btrfs_subpage *)page->private;
	const u16 tmp = btrfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->uptodate_bitmap &= ~tmp;
	ClearPageUptodate(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

static inline void btrfs_subpage_set_error(const struct btrfs_fs_info *fs_info,
					   struct page *page, u64 start,
					   u32 len)
{
	struct btrfs_subpage *subpage = (struct btrfs_subpage *)page->private;
	const u16 tmp = btrfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->error_bitmap |= tmp;
	SetPageError(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

static inline void btrfs_subpage_clear_error(const struct btrfs_fs_info *fs_info,
					     struct page *page, u64 start,
					     u32 len)
{
	struct btrfs_subpage *subpage = (struct btrfs_subpage *)page->private;
	const u16 tmp = btrfs_subpage_calc_bitmap(fs_info, page, start, len);
	unsigned long flags;

	spin_lock_irqsave(&subpage->lock, flags);
	subpage->error_bitmap &= ~tmp;
	if (subpage->error_bitmap == 0)
		ClearPageError(page);
	spin_unlock_irqrestore(&subpage->lock, flags);
}

/*
 * Unlike set/clear which is dependent on each page status, for test all bits
 * are tested in the same way.
 */
#define DECLARE_BTRFS_SUBPAGE_TEST_OP(name)				\
static inline bool btrfs_subpage_test_##name(const struct btrfs_fs_info *fs_info, \
			struct page *page, u64 start, u32 len)		\
{									\
	struct btrfs_subpage *subpage = (struct btrfs_subpage *)page->private; \
	const u16 tmp = btrfs_subpage_calc_bitmap(fs_info, page, start, len); \
	unsigned long flags;						\
	bool ret;							\
									\
	spin_lock_irqsave(&subpage->lock, flags);			\
	ret = ((subpage->name##_bitmap & tmp) == tmp);			\
	spin_unlock_irqrestore(&subpage->lock, flags);			\
	return ret;							\
}
DECLARE_BTRFS_SUBPAGE_TEST_OP(uptodate);
DECLARE_BTRFS_SUBPAGE_TEST_OP(error);

/*
 * Note that, in selftests (extent-io-tests), we can have empty fs_info passed
 * in.  We only test sectorsize == PAGE_SIZE cases so far, thus we can fall
 * back to regular sectorsize branch.
 */
#define DECLARE_BTRFS_PAGE_OPS(name, set_page_func, clear_page_func,	\
			       test_page_func)				\
static inline void btrfs_page_set_##name(struct btrfs_fs_info *fs_info,	\
			struct page *page, u64 start, u32 len)		\
{									\
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE) {	\
		set_page_func(page);					\
		return;							\
	}								\
	btrfs_subpage_set_##name(fs_info, page, start, len);		\
}									\
static inline void btrfs_page_clear_##name(const struct btrfs_fs_info *fs_info, \
			struct page *page, u64 start, u32 len)		\
{									\
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE) {	\
		clear_page_func(page);					\
		return;							\
	}								\
	btrfs_subpage_clear_##name(fs_info, page, start, len);		\
}									\
static inline bool btrfs_page_test_##name(const struct btrfs_fs_info *fs_info, \
			struct page *page, u64 start, u32 len)		\
{									\
	if (unlikely(!fs_info) || fs_info->sectorsize == PAGE_SIZE)	\
		return test_page_func(page);				\
	return btrfs_subpage_test_##name(fs_info, page, start, len);	\
}
DECLARE_BTRFS_PAGE_OPS(uptodate, SetPageUptodate, ClearPageUptodate,PageUptodate);
DECLARE_BTRFS_PAGE_OPS(error, SetPageError, ClearPageError, PageError);

#endif
