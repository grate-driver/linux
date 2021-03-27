// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "tegra-partition: " fmt

#include <linux/blkdev.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/sizes.h>

#include <linux/mmc/blkdev.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>

#include <soc/tegra/common.h>

#include "check.h"

static const struct of_device_id tegra_sdhci_match[] = {
	{ .compatible = "nvidia,tegra20-sdhci", },
	{ .compatible = "nvidia,tegra30-sdhci", },
	{ .compatible = "nvidia,tegra114-sdhci", },
	{ .compatible = "nvidia,tegra124-sdhci", },
	{}
};

int tegra_partition_forced_gpt(struct parsed_partitions *state)
{
	struct gendisk *disk = state->disk;
	struct block_device *bdev = disk->part0;
	struct mmc_card *card = mmc_bdev_to_card(bdev);
	int ret, boot_offset;

	if (!soc_is_tegra())
		return 0;

	/* filter out unrelated and untested boot sources */
	if (!card || card->ext_csd.rev < 3 ||
	    !mmc_card_is_blockaddr(card) ||
	     mmc_card_is_removable(card->host) ||
	     bdev_logical_block_size(bdev) != SZ_512 ||
	    !of_match_node(tegra_sdhci_match, card->host->parent->of_node)) {
		pr_debug("%s: unexpected boot source\n", disk->disk_name);
		return 0;
	}

	/*
	 * eMMC storage has two special boot partitions in addition to the
	 * main one.  NVIDIA's bootloader linearizes eMMC boot0->boot1->main
	 * accesses, this means that the partition table addresses are shifted
	 * by the size of boot partitions.  In accordance with the eMMC
	 * specification, the boot partition size is calculated as follows:
	 *
	 *	boot partition size = 128K byte x BOOT_SIZE_MULT
	 *
	 * This function returns number of sectors occupied by the both boot
	 * partitions.
	 */
	boot_offset = card->ext_csd.raw_boot_mult * SZ_128K /
		      SZ_512 * MMC_NUM_BOOT_PARTITION;

	/*
	 * The fixed GPT entry address is calculated like this:
	 *
	 * gpt_sector = ext_csd.sectors_num - ext_csd.boot_sectors_num - 1
	 *
	 * This algorithm is defined by NVIDIA and used by Android devices.
	 */
	state->force_gpt_sector  = get_capacity(disk);
	state->force_gpt_sector -= boot_offset + 1;

	ret = efi_partition(state);
	state->force_gpt_sector = 0;

	return ret;
}
