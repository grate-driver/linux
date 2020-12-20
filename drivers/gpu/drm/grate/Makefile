# SPDX-License-Identifier: GPL-2.0
ccflags-y := -I $(srctree)/$(src)
ccflags-y += -I $(srctree)/$(src)/uapi
ccflags-y += -I $(srctree)/drivers/gpu/host1x-grate/soc/hw
ccflags-$(CONFIG_DRM_TEGRA_DEBUG) += -DDEBUG

tegra-drm-y := \
	drm.o \
	gem.o \
	fb.o \
	dp.o \
	hub.o \
	plane.o \
	dc.o \
	output.o \
	rgb.o \
	hda.o \
	hdmi.o \
	mipi-phy.o \
	dsi.o \
	sor.o \
	dpaux.o \
	gr2d.o \
	gr3d.o \
	falcon.o \
	vic.o \
	trace.o \
	channel.o \
	client.o \
	gart.o \
	uapi/debug.o \
	uapi/job_v1.o \
	uapi/job_v2.o \
	uapi/patching.o \
	uapi/scheduler.o \
	uapi/uapi.o

obj-$(CONFIG_DRM_TEGRA) += tegra-drm.o
