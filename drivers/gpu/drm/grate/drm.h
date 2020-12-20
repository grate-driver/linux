/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Avionic Design GmbH
 * Copyright (C) 2012-2013 NVIDIA CORPORATION.  All rights reserved.
 */

#ifndef HOST1X_DRM_H
#define HOST1X_DRM_H 1

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/host1x-grate.h>
#include <linux/iommu.h>
#include <linux/iova.h>
#include <linux/gpio/consumer.h>

#include <drm/drm_atomic.h>
#include <drm/drm_bridge.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_fixed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_syncobj.h>
#include <drm/gpu_scheduler.h>

#include <uapi/drm/grate_drm.h>

#include "gem.h"
#include "channel.h"
#include "client.h"
#include "hub.h"
#include "trace.h"

#define GRATE_KERNEL_DRM_VERSION	(99991 + 6)

struct reset_control;

#ifdef CONFIG_DRM_FBDEV_EMULATION
struct tegra_fbdev {
	struct drm_fb_helper base;
	struct drm_framebuffer *fb;
};
#endif

struct tegra_drm {
	struct drm_device *drm;

	struct iommu_domain *domain;
	struct iommu_group *group;
	struct mutex mm_lock;
	struct drm_mm mm;
	struct list_head mm_eviction_list;

	struct {
		struct iova_domain domain;
		unsigned long shift;
		unsigned long limit;
		bool inited : 1;
	} carveout;

	struct list_head clients;
	struct list_head channels;

	spinlock_t context_lock;
	struct idr drm_contexts;

#ifdef CONFIG_DRM_FBDEV_EMULATION
	struct tegra_fbdev *fbdev;
#endif

	unsigned int pitch_align;

	struct tegra_display_hub *hub;

	struct completion gart_free_up;

	bool has_gart;
};

struct tegra_drm_file {
	struct drm_sched_entity *sched_entities;
	struct idr uapi_v1_contexts;
	atomic_t num_active_jobs;
	u64 drm_context;
};

int tegra_drm_init(struct tegra_drm *tegra, struct drm_device *drm);
int tegra_drm_exit(struct tegra_drm *tegra);

void *tegra_drm_alloc(struct tegra_drm *tegra, size_t size, dma_addr_t *iova);
void tegra_drm_free(struct tegra_drm *tegra, size_t size, void *virt,
		    dma_addr_t iova);

struct cec_notifier;

struct tegra_output {
	struct device_node *of_node;
	struct device *dev;

	struct drm_bridge *bridge;
	struct drm_panel *panel;
	struct i2c_adapter *ddc;
	const struct edid *edid;
	struct cec_notifier *cec;
	unsigned int hpd_irq;
	struct gpio_desc *hpd_gpio;

	struct drm_encoder encoder;
	struct drm_connector connector;
};

static inline struct tegra_output *encoder_to_output(struct drm_encoder *e)
{
	return container_of(e, struct tegra_output, encoder);
}

static inline struct tegra_output *connector_to_output(struct drm_connector *c)
{
	return container_of(c, struct tegra_output, connector);
}

/* from output.c */
int tegra_output_probe(struct tegra_output *output);
void tegra_output_remove(struct tegra_output *output);
int tegra_output_init(struct drm_device *drm, struct tegra_output *output);
void tegra_output_exit(struct tegra_output *output);
void tegra_output_find_possible_crtcs(struct tegra_output *output,
				      struct drm_device *drm);
int tegra_output_suspend(struct tegra_output *output);
int tegra_output_resume(struct tegra_output *output);

int tegra_output_connector_get_modes(struct drm_connector *connector);
enum drm_connector_status
tegra_output_connector_detect(struct drm_connector *connector, bool force);
void tegra_output_connector_destroy(struct drm_connector *connector);

/* from dpaux.c */
struct drm_dp_aux *drm_dp_aux_find_by_of_node(struct device_node *np);
enum drm_connector_status drm_dp_aux_detect(struct drm_dp_aux *aux);
int drm_dp_aux_attach(struct drm_dp_aux *aux, struct tegra_output *output);
int drm_dp_aux_detach(struct drm_dp_aux *aux);
int drm_dp_aux_enable(struct drm_dp_aux *aux);
int drm_dp_aux_disable(struct drm_dp_aux *aux);

/* from fb.c */
struct tegra_bo *tegra_fb_get_plane(struct drm_framebuffer *framebuffer,
				    unsigned int index);
bool tegra_fb_is_bottom_up(struct drm_framebuffer *framebuffer);
int tegra_fb_get_tiling(struct drm_framebuffer *framebuffer,
			struct tegra_bo_tiling *tiling);
struct drm_framebuffer *tegra_fb_create(struct drm_device *drm,
					struct drm_file *file,
					const struct drm_mode_fb_cmd2 *cmd);
int tegra_drm_fb_prepare(struct drm_device *drm);
void tegra_drm_fb_free(struct drm_device *drm);
int tegra_drm_fb_init(struct drm_device *drm);
void tegra_drm_fb_exit(struct drm_device *drm);

extern struct platform_driver tegra_display_hub_driver;
extern struct platform_driver tegra_dc_driver;
extern struct platform_driver tegra_hdmi_driver;
extern struct platform_driver tegra_dsi_driver;
extern struct platform_driver tegra_dpaux_driver;
extern struct platform_driver tegra_sor_driver;
extern struct platform_driver tegra_gr2d_driver;
extern struct platform_driver tegra_gr3d_driver;
extern struct platform_driver tegra_vic_driver;

#endif /* HOST1X_DRM_H */
