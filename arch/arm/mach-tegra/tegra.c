// SPDX-License-Identifier: GPL-2.0-only
/*
 * NVIDIA Tegra SoC device tree board support
 *
 * Copyright (C) 2011, 2013, NVIDIA Corporation
 * Copyright (C) 2010 Secret Lab Technologies, Ltd.
 * Copyright (C) 2010 Google, Inc.
 */

#include <linux/clk.h>
#include <linux/clk/tegra.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pda_power.h>
#include <linux/platform_device.h>
#include <linux/serial_8250.h>
#include <linux/slab.h>
#include <linux/sys_soc.h>
#include <linux/usb/tegra_usb_phy.h>

#include <linux/firmware/trusted_foundations.h>

#include <soc/tegra/bootdata.h>
#include <soc/tegra/fuse.h>
#include <soc/tegra/partition.h>
#include <soc/tegra/pmc.h>

#include <asm/firmware.h>
#include <asm/hardware/cache-l2x0.h>
#include <asm/mach/arch.h>
#include <asm/mach/time.h>
#include <asm/mach-types.h>
#include <asm/psci.h>
#include <asm/setup.h>

#include "board.h"
#include "common.h"
#include "iomap.h"
#include "pm.h"
#include "reset.h"
#include "sleep.h"

/*
 * Storage for debug-macro.S's state.
 *
 * This must be in .data not .bss so that it gets initialized each time the
 * kernel is loaded. The data is declared here rather than debug-macro.S so
 * that multiple inclusions of debug-macro.S point at the same data.
 */
u32 tegra_uart_config[3] = {
	/* Debug UART initialization required */
	1,
	/* Debug UART physical address */
	0,
	/* Debug UART virtual address */
	0,
};

static void __init tegra_boot_config_table_init(void)
{
	struct tegra30_boot_config_table __iomem *t30_bct;
	struct tegra20_boot_config_table __iomem *t20_bct;
	struct tegra20_boot_info_table   __iomem *t20_bit;
	u32 iram_end   = TEGRA_IRAM_BASE + TEGRA_IRAM_SIZE;
	u32 iram_start = TEGRA_IRAM_BASE;
	u32 pt_addr, pt_size, bct_size;

	t20_bit = IO_ADDRESS(TEGRA_IRAM_BASE);

	if (of_machine_is_compatible("nvidia,tegra20")) {
		bct_size = sizeof(*t20_bct);

		if (t20_bit->bct_size != bct_size ||
		    t20_bit->bct_ptr < iram_start ||
		    t20_bit->bct_ptr > iram_end - bct_size)
			return;

		t20_bct = IO_ADDRESS(t20_bit->bct_ptr);

		if (t20_bct->boot_data_version != TEGRA_BOOTDATA_VERSION_T20)
			return;

		pt_addr = t20_bct->partition_table_logical_sector_address;
		pt_size = t20_bct->partition_table_num_logical_sectors;

	} else if (of_machine_is_compatible("nvidia,tegra30")) {
		bct_size = sizeof(*t30_bct);

		if (t20_bit->bct_size != bct_size ||
		    t20_bit->bct_ptr < iram_start ||
		    t20_bit->bct_ptr > iram_end - bct_size)
			return;

		t30_bct = IO_ADDRESS(t20_bit->bct_ptr);

		if (t30_bct->boot_data_version != TEGRA_BOOTDATA_VERSION_T30)
			return;

		pt_addr = t30_bct->partition_table_logical_sector_address;
		pt_size = t30_bct->partition_table_num_logical_sectors;
	} else {
		return;
	}

	pr_info("%s: BCT found in IRAM\n", __func__);

	tegra_partition_table_setup(pt_addr, pt_size);
}

static void __init tegra_init_early(void)
{
	of_register_trusted_foundations();
	tegra_boot_config_table_init();
	tegra_cpu_reset_handler_init();
	call_firmware_op(l2x0_init);
}

static void __init tegra_dt_init_irq(void)
{
	tegra_init_irq();
	irqchip_init();
}

static void __init tegra_dt_init(void)
{
	struct device *parent = tegra_soc_device_register();

	of_platform_default_populate(NULL, NULL, parent);
}

static void __init tegra_dt_init_late(void)
{
	tegra_init_suspend();

	if (IS_ENABLED(CONFIG_ARCH_TEGRA_2x_SOC) &&
	    of_machine_is_compatible("compal,paz00"))
		tegra_paz00_wifikill_init();

	if (IS_ENABLED(CONFIG_ARCH_TEGRA_2x_SOC) &&
	    of_machine_is_compatible("nvidia,tegra20"))
		platform_device_register_simple("tegra20-cpufreq", -1, NULL, 0);

	if (IS_ENABLED(CONFIG_ARM_TEGRA_CPUIDLE) && !psci_smp_available())
		platform_device_register_simple("tegra-cpuidle", -1, NULL, 0);

	if (IS_ENABLED(CONFIG_ARCH_TEGRA_3x_SOC) &&
	    of_machine_is_compatible("nvidia,tegra30"))
		platform_device_register_simple("tegra20-cpufreq", -1, NULL, 0);
}

static const char * const tegra_dt_board_compat[] = {
	"nvidia,tegra124",
	"nvidia,tegra114",
	"nvidia,tegra30",
	"nvidia,tegra20",
	NULL
};

DT_MACHINE_START(TEGRA_DT, "NVIDIA Tegra SoC (Flattened Device Tree)")
	.l2c_aux_val	= 0x3c400000,
	.l2c_aux_mask	= 0xc20fc3ff,
	.smp		= smp_ops(tegra_smp_ops),
	.map_io		= tegra_map_common_io,
	.init_early	= tegra_init_early,
	.init_irq	= tegra_dt_init_irq,
	.init_machine	= tegra_dt_init,
	.init_late	= tegra_dt_init_late,
	.dt_compat	= tegra_dt_board_compat,
MACHINE_END
