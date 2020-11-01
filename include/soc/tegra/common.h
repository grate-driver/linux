/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2014 NVIDIA Corporation
 */

#ifndef __SOC_TEGRA_COMMON_H__
#define __SOC_TEGRA_COMMON_H__

#include <linux/types.h>

struct device;

#ifdef CONFIG_ARCH_TEGRA
bool soc_is_tegra(void);
void tegra_soc_device_sync_state(struct device *dev);
bool tegra_soc_dvfs_state_synced(void);
#else
static inline bool soc_is_tegra(void)
{
	return false;
}

static inline void tegra_soc_device_sync_state(struct device *dev)
{
}

static inline tegra_soc_dvfs_state_synced(void)
{
	return false;
}
#endif

#endif /* __SOC_TEGRA_COMMON_H__ */
