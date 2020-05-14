// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include <soc/tegra/bootdata.h>
#include <soc/tegra/common.h>

union tegra_bct_entry {
	struct tegra20_boot_config_table t20;
	struct tegra30_boot_config_table t30;
};

/*
 * spare_bct will be released once kernel is booted, hence not wasting
 * kernel space if BCT is missing. The tegra_bct can't be allocated during
 * of BCT setting up because it's too early for the slab allocator.
 */
static union tegra_bct_entry  spare_bct __initdata;
static union tegra_bct_entry *tegra_bct;

static ssize_t boot_config_table_read(struct file *filp,
				      struct kobject *kobj,
				      struct bin_attribute *bin_attr,
				      char *buf, loff_t off, size_t count)
{
	memcpy(buf, (u8 *)tegra_bct + off, count);
	return count;
}
static BIN_ATTR_RO(boot_config_table, 0);

static int __init tegra_bootdata_bct_sysfs_init(void)
{
	int err;

	if (!bin_attr_boot_config_table.size)
		return 0;

	tegra_bct = kmalloc(bin_attr_boot_config_table.size, GFP_KERNEL);
	if (!tegra_bct)
		return -ENOMEM;

	memcpy(tegra_bct, &spare_bct, bin_attr_boot_config_table.size);

	err = sysfs_create_bin_file(tegra_soc_kobj,
				    &bin_attr_boot_config_table);
	if (err)
		goto free_bct;

	return 0;

free_bct:
	kfree(tegra_bct);

	return err;
}
late_initcall(tegra_bootdata_bct_sysfs_init)

void __init tegra_bootdata_bct_setup(void __iomem *bct_ptr, size_t bct_size)
{
	memcpy_fromio(&spare_bct, bct_ptr, bct_size);
	bin_attr_boot_config_table.size = bct_size;
}
