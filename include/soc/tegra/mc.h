/*
 * Copyright (C) 2014 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __SOC_TEGRA_MC_H__
#define __SOC_TEGRA_MC_H__

#include <linux/mutex.h>
#include <linux/types.h>

struct clk;
struct device;
struct page;
struct reset_control;

struct tegra_smmu_enable {
	unsigned int reg;
	unsigned int bit;
};

struct tegra_mc_timing {
	unsigned long rate;

	u32 *emem_data;
};

/* latency allowance */
struct tegra_mc_la {
	unsigned int reg;
	unsigned int shift;
	unsigned int mask;
	unsigned int def;
};

struct tegra_mc_client {
	unsigned int id;
	const char *name;
	unsigned int swgroup;

	unsigned int fifo_size;

	struct tegra_smmu_enable smmu;
	struct tegra_mc_la la;
};

struct tegra_smmu_swgroup {
	const char *name;
	unsigned int swgroup;
	unsigned int reg;
};

struct tegra_smmu_group_soc {
	const char *name;
	const unsigned int *swgroups;
	unsigned int num_swgroups;
};

struct tegra_smmu_soc {
	const struct tegra_mc_client *clients;
	unsigned int num_clients;

	const struct tegra_smmu_swgroup *swgroups;
	unsigned int num_swgroups;

	const struct tegra_smmu_group_soc *groups;
	unsigned int num_groups;

	bool supports_round_robin_arbitration;
	bool supports_request_limit;

	unsigned int num_tlb_lines;
	unsigned int num_asids;
};

struct tegra_mc;
struct tegra_smmu;

#ifdef CONFIG_TEGRA_IOMMU_SMMU
struct tegra_smmu *tegra_smmu_probe(struct device *dev,
				    const struct tegra_smmu_soc *soc,
				    struct tegra_mc *mc);
void tegra_smmu_remove(struct tegra_smmu *smmu);
#else
static inline struct tegra_smmu *
tegra_smmu_probe(struct device *dev, const struct tegra_smmu_soc *soc,
		 struct tegra_mc *mc)
{
	return NULL;
}

static inline void tegra_smmu_remove(struct tegra_smmu *smmu)
{
}
#endif

struct tegra_mc_module {
	unsigned int hw_id;
	bool valid;
};

struct tegra_mc_soc {
	const struct tegra_mc_client *clients;
	unsigned int num_clients;

	const unsigned long *emem_regs;
	unsigned int num_emem_regs;

	unsigned int num_address_bits;
	unsigned int atom_size;

	u8 client_id_mask;

	const struct tegra_smmu_soc *smmu;

	bool tegra20;

	const struct tegra_mc_module *modules;
	unsigned int num_modules;

	u32 reg_client_ctrl;
	u32 reg_client_hotresetn;
	u32 reg_client_flush_status;
};

struct tegra_mc {
	struct device *dev;
	struct tegra_smmu *smmu;
	void __iomem *regs, *regs2;
	struct clk *clk;
	int irq;

	const struct tegra_mc_soc *soc;
	unsigned long tick;

	struct tegra_mc_timing *timings;
	unsigned int num_timings;

	struct mutex lock;
};

void tegra_mc_write_emem_configuration(struct tegra_mc *mc, unsigned long rate);
unsigned int tegra_mc_get_emem_device_count(struct tegra_mc *mc);

#define TEGRA_MEMORY_CLIENT_AVP		0
#define TEGRA_MEMORY_CLIENT_DC		1
#define TEGRA_MEMORY_CLIENT_DCB		2
#define TEGRA_MEMORY_CLIENT_EPP		3
#define TEGRA_MEMORY_CLIENT_2D		4
#define TEGRA_MEMORY_CLIENT_HOST1X	5
#define TEGRA_MEMORY_CLIENT_ISP		6
#define TEGRA_MEMORY_CLIENT_MPCORE	7
#define TEGRA_MEMORY_CLIENT_MPCORELP	8
#define TEGRA_MEMORY_CLIENT_MPEA	9
#define TEGRA_MEMORY_CLIENT_MPEB	10
#define TEGRA_MEMORY_CLIENT_MPEC	11
#define TEGRA_MEMORY_CLIENT_3D		12
#define TEGRA_MEMORY_CLIENT_3D1		13
#define TEGRA_MEMORY_CLIENT_PPCS	14
#define TEGRA_MEMORY_CLIENT_VDE		15
#define TEGRA_MEMORY_CLIENT_VI		16
#define TEGRA_MEMORY_CLIENT_AFI		17
#define TEGRA_MEMORY_CLIENT_HDA		18
#define TEGRA_MEMORY_CLIENT_SATA	19
#define TEGRA_MEMORY_CLIENT_MSENC	20
#define TEGRA_MEMORY_CLIENT_VIC		21
#define TEGRA_MEMORY_CLIENT_XUSB_HOST	22
#define TEGRA_MEMORY_CLIENT_XUSB_DEV	23
#define TEGRA_MEMORY_CLIENT_TSEC	24
#define TEGRA_MEMORY_CLIENT_SDMMC1	25
#define TEGRA_MEMORY_CLIENT_SDMMC2	26
#define TEGRA_MEMORY_CLIENT_SDMMC3	27
#define TEGRA_MEMORY_CLIENT_MAX		TEGRA_MEMORY_CLIENT_SDMMC3

#define TEGRA_MEMORY_CLIENT_3D0		TEGRA_MEMORY_CLIENT_3D
#define TEGRA_MEMORY_CLIENT_MPE		TEGRA_MEMORY_CLIENT_MPEA
#define TEGRA_MEMORY_CLIENT_NVENC	TEGRA_MEMORY_CLIENT_MSENC
#define TEGRA_MEMORY_CLIENT_ISP2	TEGRA_MEMORY_CLIENT_ISP

#ifdef CONFIG_ARCH_TEGRA
int tegra_memory_client_hot_reset(unsigned int id, struct reset_control *rst,
				  unsigned long usecs);
int tegra_memory_client_hot_reset_assert(unsigned int id,
					 struct reset_control *rst);
int tegra_memory_client_hot_reset_deassert(unsigned int id,
					   struct reset_control *rst);
#else
int tegra_memory_client_hot_reset(unsigned int id, struct reset_control *rst)
{
	return -ENOSYS;
}

int tegra_memory_client_hot_reset_assert(unsigned int id,
					 struct reset_control *rst)
{
	return -ENOSYS;
}

int tegra_memory_client_hot_reset_deassert(unsigned int id,
					   struct reset_control *rst)
{
	return -ENOSYS;
}
#endif /* CONFIG_ARCH_TEGRA */

#endif /* __SOC_TEGRA_MC_H__ */
