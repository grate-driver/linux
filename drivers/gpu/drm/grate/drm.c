// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Avionic Design GmbH
 * Copyright (C) 2012-2016 NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/bitops.h>
#include <linux/idr.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_prime.h>
#include <drm/drm_vblank.h>

#include "dc.h"
#include "drm.h"
#include "uapi.h"

#define DRIVER_NAME "tegra"
#define DRIVER_DESC "NVIDIA Tegra graphics"
#define DRIVER_DATE "20120330"
#define DRIVER_MAJOR GRATE_KERNEL_DRM_VERSION
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 0

#define CARVEOUT_SZ SZ_64M

static int tegra_atomic_check(struct drm_device *drm,
			      struct drm_atomic_state *state)
{
	int err;

	err = drm_atomic_helper_check(drm, state);
	if (err < 0)
		return err;

	return tegra_display_hub_atomic_check(drm, state);
}

static const struct drm_mode_config_funcs tegra_drm_mode_config_funcs = {
	.fb_create = tegra_fb_create,
#ifdef CONFIG_DRM_FBDEV_EMULATION
	.output_poll_changed = drm_fb_helper_output_poll_changed,
#endif
	.atomic_check = tegra_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static void tegra_atomic_post_commit(struct drm_device *drm,
				     struct drm_atomic_state *old_state)
{
	struct drm_crtc_state *old_crtc_state __maybe_unused;
	struct drm_crtc *crtc;
	unsigned int i;

	for_each_old_crtc_in_state(old_state, crtc, old_crtc_state, i)
		tegra_crtc_atomic_post_commit(crtc, old_state);
}

static void tegra_atomic_commit_tail(struct drm_atomic_state *old_state)
{
	struct drm_device *drm = old_state->dev;
	struct tegra_drm *tegra = drm->dev_private;

	if (tegra->hub) {
		drm_atomic_helper_commit_modeset_disables(drm, old_state);
		tegra_display_hub_atomic_commit(drm, old_state);
		drm_atomic_helper_commit_planes(drm, old_state, 0);
		drm_atomic_helper_commit_modeset_enables(drm, old_state);
		drm_atomic_helper_commit_hw_done(old_state);
		drm_atomic_helper_wait_for_vblanks(drm, old_state);
		drm_atomic_helper_cleanup_planes(drm, old_state);
	} else {
		drm_atomic_helper_commit_tail_rpm(old_state);
	}

	tegra_atomic_post_commit(drm, old_state);
}

static const struct drm_mode_config_helper_funcs
tegra_drm_mode_config_helpers = {
	.atomic_commit_tail = tegra_atomic_commit_tail,
};

static int tegra_drm_open(struct drm_device *drm, struct drm_file *filp)
{
	struct host1x *host = dev_get_drvdata(drm->dev->parent);
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_drm_channel *drm_channel;
	struct drm_gpu_scheduler *sched;
	struct host1x_channel *channel;
	struct tegra_drm_file *fpriv;
	int err;

	fpriv = kzalloc(sizeof(*fpriv), GFP_KERNEL);
	if (!fpriv)
		return -ENOMEM;

	filp->driver_priv = fpriv;

	/* each host1x channel has its own per-context job-queue */
	fpriv->sched_entities = kcalloc(host->soc->nb_channels,
					sizeof(*fpriv->sched_entities),
					GFP_KERNEL);
	if (!fpriv->sched_entities) {
		err = -ENOMEM;
		goto err_free_fpriv;
	}

	list_for_each_entry(drm_channel, &tegra->channels, list) {
		channel = drm_channel->channel;
		sched = &drm_channel->sched;

		err = drm_sched_entity_init(&fpriv->sched_entities[channel->id],
					    DRM_SCHED_PRIORITY_NORMAL, &sched,
					    1, NULL);
		if (err)
			goto err_destroy_sched_entities;
	}

	idr_preload(GFP_KERNEL);
	spin_lock(&tegra->context_lock);

	err = idr_alloc(&tegra->drm_contexts, fpriv, 1, 0, GFP_ATOMIC);

	spin_unlock(&tegra->context_lock);
	idr_preload_end();

	if (err < 0)
		goto err_destroy_sched_entities;

	fpriv->drm_context = err;

	idr_init(&fpriv->uapi_v1_contexts);

	return 0;

err_destroy_sched_entities:
	list_for_each_entry_continue_reverse(drm_channel, &tegra->channels,
					     list) {
		channel = drm_channel->channel;
		drm_sched_entity_destroy(&fpriv->sched_entities[channel->id]);
	}

err_free_fpriv:
	kfree(fpriv);

	return err;
}

static const struct drm_ioctl_desc tegra_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_CREATE, tegra_uapi_gem_create, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_MMAP, tegra_uapi_gem_mmap, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_SYNCPT_READ, tegra_uapi_syncpt_read, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_SYNCPT_INCR, tegra_uapi_syncpt_incr, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_SYNCPT_WAIT, tegra_uapi_syncpt_wait, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_OPEN_CHANNEL, tegra_uapi_open_channel, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_CLOSE_CHANNEL, tegra_uapi_close_channel, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_GET_SYNCPT, tegra_uapi_get_syncpt, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_SUBMIT, tegra_uapi_v1_submit, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_GET_SYNCPT_BASE, tegra_uapi_get_syncpt_base, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_SET_TILING, tegra_uapi_gem_set_tiling, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_GET_TILING, tegra_uapi_gem_get_tiling, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_SET_FLAGS, tegra_uapi_gem_set_flags, \
			  DRM_RENDER_ALLOW), \
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_GET_FLAGS, tegra_uapi_gem_get_flags, \
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_GEM_CPU_PREP, tegra_uapi_gem_cpu_prep,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_SUBMIT_V2, tegra_uapi_v2_submit,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(TEGRA_VERSION, tegra_uapi_version,
			  DRM_RENDER_ALLOW),
};

static const struct file_operations tegra_drm_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.mmap = tegra_drm_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.compat_ioctl = drm_compat_ioctl,
	.llseek = noop_llseek,
};

static int tegra_uapi_v1_contexts_cleanup(int id, void *p, void *data)
{
	struct tegra_drm_context_v1 *context = p;
	tegra_uapi_v1_free_context(context);
	return 0;
}

static void tegra_drm_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct tegra_drm_file *fpriv = file->driver_priv;
	struct tegra_drm *tegra = drm->dev_private;
	struct tegra_drm_channel *drm_channel;
	struct host1x_channel *channel;
	int val, err;

	spin_lock(&tegra->context_lock);
	idr_remove(&tegra->drm_contexts, fpriv->drm_context);
	spin_unlock(&tegra->context_lock);

	list_for_each_entry(drm_channel, &tegra->channels, list) {
		channel = drm_channel->channel;
		drm_sched_entity_destroy(&fpriv->sched_entities[channel->id]);
	}

	/* job's completion is asynchronous, see tegra_drm_work_free_job() */
	err = readx_poll_timeout(atomic_read, &fpriv->num_active_jobs,
				 val, val == 0, 100000, 30 * 1000 * 1000);
	WARN_ON_ONCE(err);

	spin_lock(&tegra->context_lock);
	idr_for_each(&fpriv->uapi_v1_contexts, tegra_uapi_v1_contexts_cleanup,
		     NULL);
	spin_unlock(&tegra->context_lock);

	idr_destroy(&fpriv->uapi_v1_contexts);

	kfree(fpriv->sched_entities);
	kfree(fpriv);
}

#ifdef CONFIG_DEBUG_FS
static int tegra_debugfs_framebuffers(struct seq_file *s, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)s->private;
	struct drm_device *drm = node->minor->dev;
	struct drm_framebuffer *fb;

	mutex_lock(&drm->mode_config.fb_lock);

	list_for_each_entry(fb, &drm->mode_config.fb_list, head) {
		seq_printf(s, "%3d: user size: %d x %d, depth %d, %d bpp, refcount %d\n",
			   fb->base.id, fb->width, fb->height,
			   fb->format->depth,
			   fb->format->cpp[0] * 8,
			   drm_framebuffer_read_refcount(fb));
	}

	mutex_unlock(&drm->mode_config.fb_lock);

	return 0;
}

static int tegra_debugfs_iova(struct seq_file *s, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)s->private;
	struct drm_device *drm = node->minor->dev;
	struct tegra_drm *tegra = drm->dev_private;
	struct drm_printer p = drm_seq_file_printer(s);

	if (tegra->domain) {
		mutex_lock(&tegra->mm_lock);
		drm_mm_print(&tegra->mm, &p);
		mutex_unlock(&tegra->mm_lock);
	}

	return 0;
}

static struct drm_info_list tegra_debugfs_list[] = {
	{ "framebuffers", tegra_debugfs_framebuffers, 0 },
	{ "iova", tegra_debugfs_iova, 0 },
};

static void tegra_debugfs_init(struct drm_minor *minor)
{
	drm_debugfs_create_files(tegra_debugfs_list,
				 ARRAY_SIZE(tegra_debugfs_list),
				 minor->debugfs_root, minor);
}
#endif

static struct drm_driver tegra_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM |
			   DRIVER_ATOMIC | DRIVER_RENDER |
			   DRIVER_SYNCOBJ,
	.open = tegra_drm_open,
	.postclose = tegra_drm_postclose,
	.lastclose = drm_fb_helper_lastclose,

#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = tegra_debugfs_init,
#endif

	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = tegra_gem_prime_import,

	.dumb_create = tegra_bo_dumb_create,

	.ioctls = tegra_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(tegra_drm_ioctls),
	.fops = &tegra_drm_fops,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

void *tegra_drm_alloc(struct tegra_drm *tegra, size_t size, dma_addr_t *dma)
{
	struct iova *alloc;
	void *virt;
	gfp_t gfp;
	int err;

	if (!tegra->carveout.inited)
		return NULL;

	if (tegra->domain)
		size = iova_align(&tegra->carveout.domain, size);
	else
		size = PAGE_ALIGN(size);

	gfp = GFP_KERNEL | __GFP_ZERO;
	if (!tegra->domain) {
		/*
		 * Many units only support 32-bit addresses, even on 64-bit
		 * SoCs. If there is no IOMMU to translate into a 32-bit IO
		 * virtual address space, force allocations to be in the
		 * lower 32-bit range.
		 */
		gfp |= GFP_DMA;
	}

	virt = (void *)__get_free_pages(gfp, get_order(size));
	if (!virt)
		return ERR_PTR(-ENOMEM);

	if (!tegra->domain) {
		/*
		 * If IOMMU is disabled, devices address physical memory
		 * directly.
		 */
		*dma = virt_to_phys(virt);
		return virt;
	}

	alloc = alloc_iova(&tegra->carveout.domain,
			   size >> tegra->carveout.shift,
			   tegra->carveout.limit, true);
	if (!alloc) {
		err = -EBUSY;
		goto free_pages;
	}

	*dma = iova_dma_addr(&tegra->carveout.domain, alloc);
	err = iommu_map(tegra->domain, *dma, virt_to_phys(virt),
			size, IOMMU_READ | IOMMU_WRITE);
	if (err < 0)
		goto free_iova;

	return virt;

free_iova:
	__free_iova(&tegra->carveout.domain, alloc);
free_pages:
	free_pages((unsigned long)virt, get_order(size));

	return ERR_PTR(err);
}

void tegra_drm_free(struct tegra_drm *tegra, size_t size, void *virt,
		    dma_addr_t dma)
{
	if (tegra->domain)
		size = iova_align(&tegra->carveout.domain, size);
	else
		size = PAGE_ALIGN(size);

	if (tegra->domain) {
		iommu_unmap(tegra->domain, dma, size);
		free_iova(&tegra->carveout.domain,
			  iova_pfn(&tegra->carveout.domain, dma));
	}

	free_pages((unsigned long)virt, get_order(size));
}

static int host1x_drm_probe(struct host1x_device *dev)
{
	struct drm_driver *driver = &tegra_drm_driver;
	struct tegra_drm *tegra;
	struct drm_device *drm;
	int err;

	drm = drm_dev_alloc(driver, &dev->dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	tegra = kzalloc(sizeof(*tegra), GFP_KERNEL);
	if (!tegra) {
		err = -ENOMEM;
		goto put;
	}

	if (iommu_present(&platform_bus_type)) {
		tegra->domain = iommu_domain_alloc(&platform_bus_type);
		if (!tegra->domain) {
			err = -ENOMEM;
			goto free;
		}

		err = iova_cache_get();
		if (err < 0)
			goto domain;
	}

	INIT_LIST_HEAD(&tegra->clients);
	INIT_LIST_HEAD(&tegra->channels);
	INIT_LIST_HEAD(&tegra->mm_eviction_list);

	mutex_init(&tegra->mm_lock);
	idr_init_base(&tegra->drm_contexts, 1);
	spin_lock_init(&tegra->context_lock);
	init_completion(&tegra->gart_free_up);

	dev_set_drvdata(&dev->dev, drm);
	drm->dev_private = tegra;
	tegra->drm = drm;

	drm_mode_config_init(drm);

	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;

	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;

	drm->mode_config.allow_fb_modifiers = true;

	drm->mode_config.normalize_zpos = true;

	drm->mode_config.funcs = &tegra_drm_mode_config_funcs;
	drm->mode_config.helper_private = &tegra_drm_mode_config_helpers;

	err = tegra_drm_fb_prepare(drm);
	if (err < 0)
		goto config;

	drm_kms_helper_poll_init(drm);

	err = host1x_device_init(dev);
	if (err < 0)
		goto fbdev;

	if (tegra->domain) {
		u64 carveout_start, carveout_end, gem_start, gem_end;
		u64 dma_mask = dma_get_mask(&dev->dev);
		dma_addr_t start, end;
		unsigned long order;
		bool need_carveout;

		start = tegra->domain->geometry.aperture_start & dma_mask;
		end = tegra->domain->geometry.aperture_end & dma_mask;

		if (of_machine_is_compatible("nvidia,tegra20"))
			tegra->has_gart = true;

		/*
		 * Carveout isn't needed on pre-Tegra124, especially on Tegra20
		 * as it uses GART that has very limited amount of IOVA space.
		 */
		if (of_machine_is_compatible("nvidia,tegra20") ||
		    of_machine_is_compatible("nvidia,tegra30") ||
		    of_machine_is_compatible("nvidia,tegra114"))
			need_carveout = false;
		else
			need_carveout = true;

		gem_start = start;
		gem_end = end;

		if (need_carveout) {
			gem_end -= CARVEOUT_SZ;
			carveout_start = gem_end + 1;
			carveout_end = end;

			order = __ffs(tegra->domain->pgsize_bitmap);
			init_iova_domain(&tegra->carveout.domain, 1UL << order,
					 carveout_start >> order);

			tegra->carveout.shift =
					iova_shift(&tegra->carveout.domain);
			tegra->carveout.limit =
					carveout_end >> tegra->carveout.shift;

			tegra->carveout.inited = 1;
		}

		drm_mm_init(&tegra->mm, gem_start, gem_end - gem_start + 1);

		DRM_DEBUG_DRIVER("IOMMU apertures:\n");
		DRM_DEBUG_DRIVER("  GEM: %#llx-%#llx\n", gem_start, gem_end);

		if (need_carveout)
			DRM_DEBUG_DRIVER("  Carveout: %#llx-%#llx\n",
					 carveout_start, carveout_end);
	}

	if (tegra->hub) {
		err = tegra_display_hub_prepare(tegra->hub);
		if (err < 0)
			goto device;
	}

	/*
	 * We don't use the drm_irq_install() helpers provided by the DRM
	 * core, so we need to set this manually in order to allow the
	 * DRM_IOCTL_WAIT_VBLANK to operate correctly.
	 */
	drm->irq_enabled = true;

	/* syncpoints are used for full 32-bit hardware VBLANK counters */
	drm->max_vblank_count = 0xffffffff;

	err = drm_vblank_init(drm, drm->mode_config.num_crtc);
	if (err < 0)
		goto hub;

	drm_mode_config_reset(drm);

	err = drm_fb_helper_remove_conflicting_framebuffers(NULL, "tegradrmfb",
							    false);
	if (err < 0)
		goto hub;

	err = tegra_drm_fb_init(drm);
	if (err < 0)
		goto hub;

	err = drm_dev_register(drm, 0);
	if (err < 0)
		goto fb;

	return 0;

fb:
	tegra_drm_fb_exit(drm);
hub:
	if (tegra->hub)
		tegra_display_hub_cleanup(tegra->hub);
device:
	if (tegra->domain) {
		drm_mm_takedown(&tegra->mm);
		if (tegra->carveout.inited)
			put_iova_domain(&tegra->carveout.domain);
		iova_cache_put();
	}

	host1x_device_exit(dev);
fbdev:
	drm_kms_helper_poll_fini(drm);
	tegra_drm_fb_free(drm);
config:
	drm_mode_config_cleanup(drm);

	idr_destroy(&tegra->drm_contexts);
	mutex_destroy(&tegra->mm_lock);
domain:
	if (tegra->domain)
		iommu_domain_free(tegra->domain);
free:
	kfree(tegra);
put:
	drm_dev_put(drm);
	return err;
}

static int host1x_drm_remove(struct host1x_device *dev)
{
	struct drm_device *drm = dev_get_drvdata(&dev->dev);
	struct tegra_drm *tegra = drm->dev_private;
	int err;

	drm_dev_unregister(drm);

	drm_kms_helper_poll_fini(drm);
	tegra_drm_fb_exit(drm);
	drm_atomic_helper_shutdown(drm);
	drm_mode_config_cleanup(drm);

	if (tegra->hub)
		tegra_display_hub_cleanup(tegra->hub);

	err = host1x_device_exit(dev);
	if (err < 0)
		dev_err(&dev->dev, "host1x device cleanup failed: %d\n", err);

	if (tegra->domain) {
		drm_mm_takedown(&tegra->mm);
		if (tegra->carveout.inited)
			put_iova_domain(&tegra->carveout.domain);
		iova_cache_put();
		iommu_domain_free(tegra->domain);
	}

	idr_destroy(&tegra->drm_contexts);
	mutex_destroy(&tegra->mm_lock);

	kfree(tegra);
	drm_dev_put(drm);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int host1x_drm_suspend(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	return drm_mode_config_helper_suspend(drm);
}

static int host1x_drm_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	return drm_mode_config_helper_resume(drm);
}
#endif

static SIMPLE_DEV_PM_OPS(host1x_drm_pm_ops, host1x_drm_suspend,
			 host1x_drm_resume);

static const struct of_device_id host1x_drm_subdevs[] = {
	{ .compatible = "nvidia,tegra20-dc", },
	{ .compatible = "nvidia,tegra20-hdmi", },
	{ .compatible = "nvidia,tegra20-gr2d", },
	{ .compatible = "nvidia,tegra20-gr3d", },
	{ .compatible = "nvidia,tegra30-dc", },
	{ .compatible = "nvidia,tegra30-hdmi", },
	{ .compatible = "nvidia,tegra30-gr2d", },
	{ .compatible = "nvidia,tegra30-gr3d", },
	{ .compatible = "nvidia,tegra114-dc", },
	{ .compatible = "nvidia,tegra114-dsi", },
	{ .compatible = "nvidia,tegra114-hdmi", },
	{ .compatible = "nvidia,tegra114-gr2d", },
	{ .compatible = "nvidia,tegra114-gr3d", },
	{ .compatible = "nvidia,tegra124-dc", },
	{ .compatible = "nvidia,tegra124-sor", },
	{ .compatible = "nvidia,tegra124-hdmi", },
	{ .compatible = "nvidia,tegra124-dsi", },
	{ .compatible = "nvidia,tegra124-vic", },
	{ .compatible = "nvidia,tegra132-dsi", },
	{ .compatible = "nvidia,tegra210-dc", },
	{ .compatible = "nvidia,tegra210-dsi", },
	{ .compatible = "nvidia,tegra210-sor", },
	{ .compatible = "nvidia,tegra210-sor1", },
	{ .compatible = "nvidia,tegra210-vic", },
	{ .compatible = "nvidia,tegra186-display", },
	{ .compatible = "nvidia,tegra186-dc", },
	{ .compatible = "nvidia,tegra186-sor", },
	{ .compatible = "nvidia,tegra186-sor1", },
	{ .compatible = "nvidia,tegra186-vic", },
	{ .compatible = "nvidia,tegra194-display", },
	{ .compatible = "nvidia,tegra194-dc", },
	{ .compatible = "nvidia,tegra194-sor", },
	{ .compatible = "nvidia,tegra194-vic", },
	{ /* sentinel */ }
};

static struct host1x_driver host1x_drm_driver = {
	.driver = {
		.name = "drm",
		.pm = &host1x_drm_pm_ops,
	},
	.probe = host1x_drm_probe,
	.remove = host1x_drm_remove,
	.subdevs = host1x_drm_subdevs,
};

static struct platform_driver * const drivers[] = {
	&tegra_display_hub_driver,
	&tegra_dc_driver,
	&tegra_hdmi_driver,
	&tegra_dsi_driver,
	&tegra_dpaux_driver,
	&tegra_sor_driver,
	&tegra_gr2d_driver,
	&tegra_gr3d_driver,
	&tegra_vic_driver,
};

static int __init host1x_drm_init(void)
{
	int err;

	err = host1x_driver_register(&host1x_drm_driver);
	if (err < 0)
		return err;

	err = platform_register_drivers(drivers, ARRAY_SIZE(drivers));
	if (err < 0)
		goto unregister_host1x;

	return 0;

unregister_host1x:
	host1x_driver_unregister(&host1x_drm_driver);
	return err;
}
module_init(host1x_drm_init);

static void __exit host1x_drm_exit(void)
{
	platform_unregister_drivers(drivers, ARRAY_SIZE(drivers));
	host1x_driver_unregister(&host1x_drm_driver);
}
module_exit(host1x_drm_exit);

MODULE_AUTHOR("Thierry Reding <thierry.reding@avionic-design.de>");
MODULE_DESCRIPTION("NVIDIA Tegra DRM driver");
MODULE_LICENSE("GPL v2");
