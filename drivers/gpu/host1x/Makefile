# SPDX-License-Identifier: GPL-2.0
ccflags-y := -Idrivers/gpu/host1x

host1x-drv-y = \
	buffer_object.o \
	bus.o \
	debug.o \
	dma_pool.o \
	host1x.o \
	iommu.o \
	mipi.o

# fence must be built-in because host1x-module could be unloaded while
# some other driver holds the exported fence
obj-$(CONFIG_TEGRA_HOST1X) = fence.o

host1x-drv-$(CONFIG_TEGRA_HOST1X01) += soc/host1x01.o
host1x-drv-$(CONFIG_TEGRA_HOST1X02) += soc/host1x02.o
host1x-drv-$(CONFIG_TEGRA_HOST1X04) += soc/host1x04.o
host1x-drv-$(CONFIG_TEGRA_HOST1X05) += soc/host1x05.o
host1x-drv-$(CONFIG_TEGRA_HOST1X06) += soc/host1x06.o
host1x-drv-$(CONFIG_TEGRA_HOST1X07) += soc/host1x07.o

obj-$(CONFIG_TEGRA_HOST1X_DRV) += host1x-drv.o
