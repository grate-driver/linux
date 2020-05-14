/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_TEGRA_BOOTDATA_H__
#define __SOC_TEGRA_BOOTDATA_H__

#include <linux/compiler.h>
#include <linux/types.h>

#define TEGRA_BOOTDATA_VERSION_T20	NVBOOT_BOOTDATA_VERSION(0x2, 0x1)
#define TEGRA_BOOTDATA_VERSION_T30	NVBOOT_BOOTDATA_VERSION(0x3, 0x1)

#define NVBOOT_BOOTDATA_VERSION(a, b)	((((a) & 0xffff) << 16) | \
					  ((b) & 0xffff))
#define NVBOOT_CMAC_AES_HASH_LENGTH	4

struct tegra20_boot_info_table {
	u32 unused_data1[14];
	u32 bct_size;
	u32 bct_ptr;
} __packed;

struct tegra20_boot_config_table {
	u32 crypto_hash[NVBOOT_CMAC_AES_HASH_LENGTH];
	u32 random_aes_blk[NVBOOT_CMAC_AES_HASH_LENGTH];
	u32 boot_data_version;
	u32 unused_data1[712];
	u32 unused_consumer_data1;
	u16 partition_table_logical_sector_address;
	u16 partition_table_num_logical_sectors;
	u32 unused_consumer_data[294];
	u32 unused_data[3];
} __packed;

struct tegra30_boot_config_table {
	u32 crypto_hash[NVBOOT_CMAC_AES_HASH_LENGTH];
	u32 random_aes_blk[NVBOOT_CMAC_AES_HASH_LENGTH];
	u32 boot_data_version;
	u32 unused_data1[1016];
	u32 unused_consumer_data1;
	u16 partition_table_logical_sector_address;
	u16 partition_table_num_logical_sectors;
	u32 unused_consumer_data[502];
	u32 unused_data[3];
} __packed;

void tegra_bootdata_bct_setup(void __iomem *bct_ptr, size_t bct_size);

#endif /* __SOC_TEGRA_BOOTDATA_H__ */
