// SPDX-License-Identifier: GPL-2.0
/*
 * NVIDIA Tegra Partition Table
 *
 * Copyright (C) 2020 GRATE-DRIVER project
 * Copyright (C) 2020 Dmitry Osipenko <digetx@gmail.com>
 *
 * Credits for the partition table format:
 *
 *   Andrey Danin <danindrey@mail.ru>       (Toshiba AC100 TegraPT format)
 *   Gilles Grandou <gilles@grandou.net>    (Toshiba AC100 TegraPT format)
 *   Ryan Grachek <ryan@edited.us>          (Google TV "Molly" TegraPT format)
 *   Stephen Warren <swarren@wwwdotorg.org> (Useful suggestions about eMMC/etc)
 */

#define pr_fmt(fmt) "tegra-partition: " fmt

#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/mmc/blkdev.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>

#include <soc/tegra/common.h>
#include <soc/tegra/partition.h>

#include "check.h"

#define TEGRA_PT_SECTOR_SIZE(ptp)	((ptp)->logical_sector_size / SZ_512)
#define TEGRA_PT_SECTOR(ptp, s)		((s) * TEGRA_PT_SECTOR_SIZE(ptp))

#define TEGRA_PT_HEADER_SIZE						\
	(sizeof(struct tegra_partition_header_insecure) +		\
	 sizeof(struct tegra_partition_header_secure))			\

#define TEGRA_PT_MAX_PARTITIONS(ptp)					\
	(((ptp)->logical_sector_size - TEGRA_PT_HEADER_SIZE) /		\
	 sizeof(struct tegra_partition))

#define TEGRA_PT_ERR(ptp, fmt, ...)					\
	pr_debug("%s: " fmt,						\
		 (ptp)->state->bdev->bd_disk->disk_name, ##__VA_ARGS__)	\

#define TEGRA_PT_PARSE_ERR(ptp, fmt, ...)				\
	TEGRA_PT_ERR(ptp, "sector %llu: invalid " fmt,			\
		     (ptp)->sector, ##__VA_ARGS__)

struct tegra_partition_table_parser {
	struct tegra_partition_table *pt;
	unsigned int logical_sector_size;
	struct parsed_partitions *state;
	bool pt_entry_checked;
	sector_t sector;
	int boot_offset;
	u32 dev_instance;
	u32 dev_id;
};

union tegra_partition_table_u {
	struct tegra_partition_table pt;
	u8 pt_parts[SZ_4K / SZ_512][SZ_512];
};

struct tegra_partition_type {
	unsigned int type;
	char *name;
};

static sector_t tegra_pt_logical_sector_address;
static sector_t tegra_pt_logical_sectors_num;

void tegra_partition_table_setup(unsigned int logical_sector_address,
				 unsigned int logical_sectors_num)
{
	tegra_pt_logical_sector_address = logical_sector_address;
	tegra_pt_logical_sectors_num    = logical_sectors_num;

	pr_info("initialized to logical sector = %llu sectors_num = %llu\n",
		tegra_pt_logical_sector_address, tegra_pt_logical_sectors_num);
}

/*
 * Some partitions are very sensitive, changing data on them may brick device.
 *
 * For more details about partitions see:
 *
 *  "https://docs.nvidia.com/jetson/l4t/Tegra Linux Driver Package Development Guide/part_config.html"
 */
static const char * const partitions_blacklist[] = {
	"BCT", "EBT", "EB2", "EKS", "GP1", "GPT", "MBR", "PT",
};

static bool tegra_partition_name_match(struct tegra_partition *p,
				       const char *name)
{
	return !strncmp(p->partition_name, name, TEGRA_PT_NAME_SIZE);
}

static bool tegra_partition_skip(struct tegra_partition *p,
				 struct tegra_partition_table_parser *ptp,
				 sector_t sector)
{
	unsigned int i;

	/* skip eMMC boot partitions */
	if (sector < ptp->boot_offset)
		return true;

	for (i = 0; i < ARRAY_SIZE(partitions_blacklist); i++) {
		if (tegra_partition_name_match(p, partitions_blacklist[i]))
			return true;
	}

	return false;
}

static const struct tegra_partition_type tegra_partition_expected_types[] = {
	{ .type = TEGRA_PT_PART_TYPE_BCT,	.name = "BCT", },
	{ .type = TEGRA_PT_PART_TYPE_EBT,	.name = "EBT", },
	{ .type = TEGRA_PT_PART_TYPE_EBT,	.name = "EB2", },
	{ .type = TEGRA_PT_PART_TYPE_PT,	.name = "PT",  },
	{ .type = TEGRA_PT_PART_TYPE_GP1,	.name = "GP1", },
	{ .type = TEGRA_PT_PART_TYPE_GPT,	.name = "GPT", },
	{ .type = TEGRA_PT_PART_TYPE_GENERIC,	.name = NULL,  },
};

static int tegra_partition_type_valid(struct tegra_partition_table_parser *ptp,
				      struct tegra_partition *p)
{
	const struct tegra_partition_type *ptype;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(tegra_partition_expected_types); i++) {
		ptype = &tegra_partition_expected_types[i];

		if (ptype->name && !tegra_partition_name_match(p, ptype->name))
			continue;

		if (p->part_info.partition_type == ptype->type)
			return 0;

		/*
		 * Unsure about all possible types, let's emit error and
		 * allow to continue for now.
		 */
		if (!ptype->name)
			return 1;
	}

	return -1;
}

static bool tegra_partition_valid(struct tegra_partition_table_parser *ptp,
				  struct tegra_partition *p,
				  struct tegra_partition *prev,
				  sector_t sector,
				  sector_t size)
{
	struct tegra_partition_info *prev_pi = &prev->part_info;
	sector_t sect_end = TEGRA_PT_SECTOR(ptp,
					    prev_pi->logical_sector_address +
					    prev_pi->logical_sectors_num);
	char *type, name[2][TEGRA_PT_NAME_SIZE + 1];
	int err;

	strscpy(name[0], p->partition_name,    sizeof(name[0]));
	strscpy(name[1], prev->partition_name, sizeof(name[1]));

	/* validate expected partition name/type */
	err = tegra_partition_type_valid(ptp, p);
	if (err) {
		TEGRA_PT_PARSE_ERR(ptp, "partition_type: [%s] partition_type=%u\n",
				   name[0], p->part_info.partition_type);
		if (err < 0)
			return false;

		TEGRA_PT_ERR(ptp, "continuing, please update list of expected types\n");
	}

	/* validate partition table BCT addresses */
	if (tegra_partition_name_match(p, "PT")) {
		if (sector != TEGRA_PT_SECTOR(ptp, tegra_pt_logical_sector_address) &&
		    size   != TEGRA_PT_SECTOR(ptp, tegra_pt_logical_sectors_num)) {
			TEGRA_PT_PARSE_ERR(ptp, "PT location: sector=%llu size=%llu\n",
					   sector, size);
			return false;
		}

		if (ptp->pt_entry_checked) {
			TEGRA_PT_PARSE_ERR(ptp, "(duplicated) PT\n");
			return false;
		}

		ptp->pt_entry_checked = true;
	}

	if (sector + size < sector) {
		TEGRA_PT_PARSE_ERR(ptp, "size: [%s] integer overflow sector=%llu size=%llu\n",
				   name[0], sector, size);
		return false;
	}

	/* validate allocation_policy=sequential (absolute unsupported) */
	if (p != prev && sect_end > sector) {
		TEGRA_PT_PARSE_ERR(ptp, "allocation_policy: [%s] end=%llu [%s] sector=%llu size=%llu\n",
				   name[1], sect_end, name[0], sector, size);
		return false;
	}

	if (ptp->dev_instance != p->mount_info.device_instance) {
		TEGRA_PT_PARSE_ERR(ptp, "device_instance: [%s] device_instance=%u|%u\n",
				   name[0], ptp->dev_instance,
				   p->mount_info.device_instance);
		return false;
	}

	if (ptp->dev_id != p->mount_info.device_id) {
		TEGRA_PT_PARSE_ERR(ptp, "device_id: [%s] device_id=%u|%u\n",
				   name[0], ptp->dev_id,
				   p->mount_info.device_id);
		return false;
	}

	if (p->partition_id > 127) {
		TEGRA_PT_PARSE_ERR(ptp, "partition_id: [%s] partition_id=%u\n",
				   name[0], p->partition_id);
		return false;
	}

	sect_end = get_capacity(ptp->state->bdev->bd_disk);

	/* eMMC boot partitions are below ptp->boot_offset */
	if (sector < ptp->boot_offset) {
		sect_end += ptp->boot_offset;
		type = "boot";
	} else {
		sector -= ptp->boot_offset;
		type = "main";
	}

	/* validate size */
	if (!size || sector + size > sect_end) {
		TEGRA_PT_PARSE_ERR(ptp, "size: [%s] %s partition boot_offt=%d end=%llu sector=%llu size=%llu\n",
				   name[0], type, ptp->boot_offset, sect_end,
				   sector, size);
		return false;
	}

	return true;
}

static bool tegra_partitions_parsed(struct tegra_partition_table_parser *ptp,
				    bool check_only)
{
	struct parsed_partitions *state = ptp->state;
	struct tegra_partition_table *pt = ptp->pt;
	sector_t sector, size;
	int i, slot = 1;

	ptp->pt_entry_checked = false;

	for (i = 0; i < pt->secure.num_partitions; i++) {
		struct tegra_partition *p = &pt->partitions[i];
		struct tegra_partition *prev = &pt->partitions[max(i - 1, 0)];
		struct tegra_partition_info *pi = &p->part_info;

		if (slot == state->limit && !check_only)
			break;

		sector = TEGRA_PT_SECTOR(ptp, pi->logical_sector_address);
		size   = TEGRA_PT_SECTOR(ptp, pi->logical_sectors_num);

		if (check_only &&
		    !tegra_partition_valid(ptp, p, prev, sector, size))
			return false;

		if (check_only ||
		    tegra_partition_skip(p, ptp, sector))
			continue;

		put_partition(state, slot++, sector - ptp->boot_offset, size);
	}

	if (check_only && !ptp->pt_entry_checked) {
		TEGRA_PT_PARSE_ERR(ptp, "PT: table entry not found\n");
		return false;
	}

	return true;
}

static bool
tegra_partition_table_parsed(struct tegra_partition_table_parser *ptp)
{
	if (ptp->pt->secure.num_partitions == 0 ||
	    ptp->pt->secure.num_partitions > TEGRA_PT_MAX_PARTITIONS(ptp)) {
		TEGRA_PT_PARSE_ERR(ptp, "num_partitions=%u\n",
				   ptp->pt->secure.num_partitions);
		return false;
	}

	return tegra_partitions_parsed(ptp, true) &&
	       tegra_partitions_parsed(ptp, false);
}

static int
tegra_partition_table_insec_hdr_valid(struct tegra_partition_table_parser *ptp)
{
	if (ptp->pt->insecure.magic   != TEGRA_PT_MAGIC ||
	    ptp->pt->insecure.version != TEGRA_PT_VERSION) {
		TEGRA_PT_PARSE_ERR(ptp, "insecure header: magic=0x%llx ver=0x%x\n",
				   ptp->pt->insecure.magic,
				   ptp->pt->insecure.version);
		return 0;
	}

	return 1;
}

static int
tegra_partition_table_sec_hdr_valid(struct tegra_partition_table_parser *ptp)
{
	size_t pt_size = ptp->pt->secure.num_partitions;

	pt_size *= sizeof(ptp->pt->partitions[0]);
	pt_size += TEGRA_PT_HEADER_SIZE;

	if (ptp->pt->secure.magic   != TEGRA_PT_MAGIC ||
	    ptp->pt->secure.version != TEGRA_PT_VERSION ||
	    ptp->pt->secure.length  != ptp->pt->insecure.length ||
	    ptp->pt->secure.length  < pt_size) {
		TEGRA_PT_PARSE_ERR(ptp, "secure header: magic=0x%llx ver=0x%x length=%u|%u|%zu\n",
				   ptp->pt->secure.magic,
				   ptp->pt->secure.version,
				   ptp->pt->secure.length,
				   ptp->pt->insecure.length,
				   pt_size);
		return 0;
	}

	return 1;
}

static int
tegra_partition_table_unencrypted(struct tegra_partition_table_parser *ptp)
{
	/* AES IV, all zeros if unencrypted */
	if (ptp->pt->secure.random_data[0] || ptp->pt->secure.random_data[1] ||
	    ptp->pt->secure.random_data[2] || ptp->pt->secure.random_data[3]) {
		pr_err_once("encrypted partition table unsupported\n");
		return 0;
	}

	return 1;
}

static int tegra_read_partition_table(struct tegra_partition_table_parser *ptp)
{
	union tegra_partition_table_u *ptu = (typeof(ptu))ptp->pt;
	unsigned int i;
	Sector sect;
	void *part;

	for (i = 0; i < ptp->logical_sector_size / SZ_512; i++) {
		/*
		 * Partition table takes at maximum 4096 bytes, but
		 * read_part_sector() guarantees only that SECTOR_SIZE will
		 * be read at minimum.
		 */
		part = read_part_sector(ptp->state, ptp->sector + i, &sect);
		if (!part) {
			TEGRA_PT_ERR(ptp, "failed to read sector %llu\n",
				     ptp->sector + i);
			return 0;
		}

		memcpy(ptu->pt_parts[i], part, SZ_512);
		put_dev_sector(sect);
	}

	return 1;
}

static int tegra_partition_scan(struct tegra_partition_table_parser *ptp)
{
	sector_t start_sector, num_sectors;
	int ret = 0;

	num_sectors  = TEGRA_PT_SECTOR(ptp, tegra_pt_logical_sectors_num);
	start_sector = TEGRA_PT_SECTOR(ptp, tegra_pt_logical_sector_address);

	if (start_sector < ptp->boot_offset) {
		TEGRA_PT_ERR(ptp,
			     "scanning eMMC boot partitions unimplemented\n");
		return 0;
	}

	ptp->sector = start_sector - ptp->boot_offset;

	/*
	 * Partition table is duplicated for num_sectors.
	 * If first table is corrupted, we will try next.
	 */
	while (num_sectors--) {
		ret = tegra_read_partition_table(ptp);
		if (!ret)
			goto next_sector;

		ret = tegra_partition_table_insec_hdr_valid(ptp);
		if (!ret)
			goto next_sector;

		ret = tegra_partition_table_unencrypted(ptp);
		if (!ret)
			goto next_sector;

		ret = tegra_partition_table_sec_hdr_valid(ptp);
		if (!ret)
			goto next_sector;

		ret = tegra_partition_table_parsed(ptp);
		if (ret)
			break;
next_sector:
		ptp->sector += TEGRA_PT_SECTOR_SIZE(ptp);
	}

	return ret;
}

static const u32 tegra20_sdhci_bases[TEGRA_PT_SDHCI_DEVICE_INSTANCES] = {
	0xc8000000, 0xc8000200, 0xc8000400, 0xc8000600,
};

static const u32 tegra30_sdhci_bases[TEGRA_PT_SDHCI_DEVICE_INSTANCES] = {
	0x78000000, 0x78000200, 0x78000400, 0x78000600,
};

static const u32 tegra124_sdhci_bases[TEGRA_PT_SDHCI_DEVICE_INSTANCES] = {
	0x700b0000, 0x700b0200, 0x700b0400, 0x700b0600,
};

static const struct of_device_id tegra_sdhci_match[] = {
	{ .compatible = "nvidia,tegra20-sdhci", .data = tegra20_sdhci_bases, },
	{ .compatible = "nvidia,tegra30-sdhci", .data = tegra30_sdhci_bases, },
	{ .compatible = "nvidia,tegra114-sdhci", .data = tegra30_sdhci_bases, },
	{ .compatible = "nvidia,tegra124-sdhci", .data = tegra124_sdhci_bases, },
	{}
};

static int
tegra_partition_table_emmc_boot_offset(struct tegra_partition_table_parser *ptp)
{
	struct mmc_card *card = mmc_bdev_to_card(ptp->state->bdev);
	const struct of_device_id *matched;
	const u32 *sdhci_bases;
	const __be32 *addrp;
	u32 sdhci_base;
	unsigned int i;

	/* filter out unexpected/untested boot sources */
	if (!card || card->ext_csd.rev < 3 ||
	    !mmc_card_is_blockaddr(card) ||
	     mmc_card_is_removable(card->host) ||
	     bdev_logical_block_size(ptp->state->bdev) != SZ_512) {
		TEGRA_PT_ERR(ptp, "unexpected boot source\n");
		return -1;
	}

	/* skip everything unrelated to Tegra eMMC */
	matched = of_match_node(tegra_sdhci_match, card->host->parent->of_node);
	if (!matched)
		return -1;

	sdhci_bases = matched->data;

	/* figure out SDHCI instance ID by the base address */
	addrp = of_get_address(card->host->parent->of_node, 0, NULL, NULL);
	if (!addrp)
		return -1;

	sdhci_base = of_translate_address(card->host->parent->of_node, addrp);

	for (i = 0; i < TEGRA_PT_SDHCI_DEVICE_INSTANCES; i++) {
		if (sdhci_base == sdhci_bases[i])
			break;
	}

	if (i == TEGRA_PT_SDHCI_DEVICE_INSTANCES)
		return -1;

	ptp->dev_id       = TEGRA_PT_SDHCI_DEVICE_ID;
	ptp->dev_instance = i;

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

/*
 * Logical sector size may vary per device model and apparently there is no
 * way to get information about the size from kernel. The info is hardcoded
 * into bootloader and it doesn't tell us, so we'll just try all possible
 * well-known sizes until succeed.
 *
 * For example Samsung Galaxy Tab 10.1 uses 2K sectors. While Acer A500,
 * Nexus 7 and Ouya are using 4K sectors.
 */
static const unsigned int tegra_pt_logical_sector_sizes[] = {
	SZ_4K, SZ_2K,
};

/*
 * The 'tegraboot=<source>' command line option is provided to kernel
 * by NVIDIA's proprietary bootloader on most Tegra devices. If it isn't
 * provided, then it should be added to the cmdline via device-tree bootargs
 * or by other means.
 */
static bool tegra_boot_sdmmc;
static int __init tegra_boot_fn(char *str)
{
	tegra_boot_sdmmc = !strcmp(str, "sdmmc");
	return 1;
}
__setup("tegraboot=", tegra_boot_fn);

int tegra_partition(struct parsed_partitions *state)
{
	struct tegra_partition_table_parser ptp = {};
	unsigned int i;
	int ret;

	if (!soc_is_tegra() || !tegra_boot_sdmmc)
		return 0;

	ptp.state = state;

	ptp.boot_offset = tegra_partition_table_emmc_boot_offset(&ptp);
	if (ptp.boot_offset < 0)
		return 0;

	ptp.pt = kmalloc(SZ_4K, GFP_KERNEL);
	if (!ptp.pt)
		return 0;

	for (i = 0; i < ARRAY_SIZE(tegra_pt_logical_sector_sizes); i++) {
		ptp.logical_sector_size = tegra_pt_logical_sector_sizes[i];

		ret = tegra_partition_scan(&ptp);
		if (ret == 1) {
			strlcat(state->pp_buf, "\n", PAGE_SIZE);
			break;
		}
	}

	kfree(ptp.pt);

	return ret;
}
