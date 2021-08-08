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

#define TEGRA_PT_ERR(_state, fmt, ...)					\
	pr_debug("%s: " fmt,						\
		 (_state)->bdev->bd_disk->disk_name, ##__VA_ARGS__)

static const struct of_device_id tegra_sdhci_match[] = {
	{ .compatible = "nvidia,tegra20-sdhci", },
	{ .compatible = "nvidia,tegra30-sdhci", },
	{ .compatible = "nvidia,tegra114-sdhci", },
	{ .compatible = "nvidia,tegra124-sdhci", },
	{}
};

static int
tegra_partition_table_emmc_boot_offset(struct parsed_partitions *state)
{
	struct mmc_card *card = mmc_bdev_to_card(state->bdev);

	/* filter out unrelated and untested boot sources */
	if (!card || card->ext_csd.rev < 3 ||
	    !mmc_card_is_blockaddr(card) ||
	     mmc_card_is_removable(card->host) ||
	     bdev_logical_block_size(state->bdev) != SZ_512 ||
	    !of_match_node(tegra_sdhci_match, card->host->parent->of_node)) {
		TEGRA_PT_ERR(state, "unexpected boot source\n");
		return -1;
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
	return card->ext_csd.raw_boot_mult * SZ_128K /
	       SZ_512 * MMC_NUM_BOOT_PARTITION;
}

int tegra_partition_forced_gpt(struct parsed_partitions *state)
{
	int ret, boot_offset;

	if (!soc_is_tegra())
		return 0;

	boot_offset = tegra_partition_table_emmc_boot_offset(state);
	if (boot_offset < 0)
		return 0;

	/*
	 * The fixed GPT entry address is calculated like this:
	 *
	 * gpt_sector = ext_csd.sectors_num - ext_csd.boot_sectors_num - 1
	 *
	 * This algorithm is defined by NVIDIA and used by Android devices.
	 */
	state->force_gpt_sector  = get_capacity(state->bdev->bd_disk);
	state->force_gpt_sector -= boot_offset + 1;

	ret = efi_partition(state);
	state->force_gpt_sector = 0;

	return ret;
}
