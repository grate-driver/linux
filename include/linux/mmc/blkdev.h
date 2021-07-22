/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/include/linux/mmc/blkdev.h
 */
#ifndef LINUX_MMC_BLOCK_DEVICE_H
#define LINUX_MMC_BLOCK_DEVICE_H

struct block_device;
struct mmc_card;

struct mmc_card *mmc_bdev_to_card(struct block_device *bdev);

#endif /* LINUX_MMC_BLOCK_DEVICE_H */
