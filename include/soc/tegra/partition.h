/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_TEGRA_PARTITION_H__
#define __SOC_TEGRA_PARTITION_H__

#include <linux/compiler.h>
#include <linux/types.h>

#define TEGRA_PT_MAGIC				0xffffffff8f9e8d8bULL
#define TEGRA_PT_VERSION			0x100
#define TEGRA_PT_AES_HASH_SIZE			4
#define TEGRA_PT_NAME_SIZE			4

#define TEGRA_PT_SDHCI_DEVICE_ID		18
#define TEGRA_PT_SDHCI_DEVICE_INSTANCES		4

#define TEGRA_PT_PART_TYPE_BCT			1
#define TEGRA_PT_PART_TYPE_EBT			2
#define TEGRA_PT_PART_TYPE_PT			3
#define TEGRA_PT_PART_TYPE_GENERIC		6
#define TEGRA_PT_PART_TYPE_GP1			9
#define TEGRA_PT_PART_TYPE_GPT			10

struct tegra_partition_mount_info {
	u32 device_id;
	u32 device_instance;
	u32 device_attr;
	u8  mount_path[TEGRA_PT_NAME_SIZE];
	u32 file_system_type;
	u32 file_system_attr;
} __packed;

struct tegra_partition_info {
	u32 partition_attr;
	u32 __pad1;
	u64 logical_sector_address;
	u64 logical_sectors_num;
	u64 physical_sector_address;
	u64 physical_sectors_num;
	u32 partition_type;
	u32 __pad2;
} __packed;

struct tegra_partition {
	u32 partition_id;
	u8  partition_name[TEGRA_PT_NAME_SIZE];
	struct tegra_partition_mount_info mount_info;
	struct tegra_partition_info part_info;
} __packed;

struct tegra_partition_header_insecure {
	u64 magic;
	u32 version;
	u32 length;
	u32 signature[TEGRA_PT_AES_HASH_SIZE];
} __packed;

struct tegra_partition_header_secure {
	u32 random_data[TEGRA_PT_AES_HASH_SIZE];
	u64 magic;
	u32 version;
	u32 length;
	u32 num_partitions;
	u32 __pad;
} __packed;

struct tegra_partition_table {
	struct tegra_partition_header_insecure insecure;
	struct tegra_partition_header_secure secure;
	struct tegra_partition partitions[];
} __packed;

#ifdef CONFIG_TEGRA_PARTITION
void tegra_partition_table_setup(unsigned int logical_sector_address,
				 unsigned int logical_sectors_num);
#else
static inline void
tegra_partition_table_setup(unsigned int logical_sector_address,
			    unsigned int logical_sectors_num)
{
}
#endif

#endif /* __SOC_TEGRA_PARTITION_H__ */
