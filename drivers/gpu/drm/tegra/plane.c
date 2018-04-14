// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017 NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/interconnect.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_plane_helper.h>

#include "dc.h"
#include "gart.h"
#include "plane.h"

static void tegra_plane_destroy(struct drm_plane *plane)
{
	struct tegra_plane *p = to_tegra_plane(plane);

	if (p->csc_default)
		drm_property_blob_put(p->csc_default);

	drm_plane_cleanup(plane);
	kfree(p);
}

static void tegra_plane_reset(struct drm_plane *plane)
{
	struct tegra_plane *p = to_tegra_plane(plane);
	struct tegra_plane_state *state;

	if (plane->state)
		__drm_atomic_helper_plane_destroy_state(plane->state);

	kfree(plane->state);
	plane->state = NULL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state) {
		plane->state = &state->base;
		plane->state->plane = plane;
		plane->state->zpos = p->index;
		plane->state->normalized_zpos = p->index;

		if (p->csc_default)
			state->csc_blob = drm_property_blob_get(p->csc_default);
	}
}

static struct drm_plane_state *
tegra_plane_atomic_duplicate_state(struct drm_plane *plane)
{
	struct tegra_plane_state *state = to_tegra_plane_state(plane->state);
	struct tegra_plane_state *copy;
	unsigned int i;

	copy = kmalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &copy->base);
	copy->tiling = state->tiling;
	copy->format = state->format;
	copy->swap = state->swap;
	copy->reflect_x = state->reflect_x;
	copy->reflect_y = state->reflect_y;
	copy->opaque = state->opaque;
	copy->peak_memory_bandwidth = state->peak_memory_bandwidth;
	copy->avg_memory_bandwidth = state->avg_memory_bandwidth;

	for (i = 0; i < 2; i++)
		copy->blending[i] = state->blending[i];

	if (state->csc_blob)
		copy->csc_blob = drm_property_blob_get(state->csc_blob);
	else
		copy->csc_blob = NULL;

	return &copy->base;
}

static void tegra_plane_atomic_destroy_state(struct drm_plane *plane,
					     struct drm_plane_state *state)
{
	struct tegra_plane_state *tegra = to_tegra_plane_state(state);

	if (tegra->csc_blob)
		drm_property_blob_put(tegra->csc_blob);

	__drm_atomic_helper_plane_destroy_state(state);
	kfree(state);
}

static int tegra_plane_set_property(struct drm_plane *plane,
				    struct drm_plane_state *state,
				    struct drm_property *property,
				    uint64_t value)
{
	struct tegra_plane_state *tegra_state = to_tegra_plane_state(state);
	struct tegra_plane *tegra = to_tegra_plane(plane);
	struct drm_property_blob *blob;

	if (property == tegra->props.csc_blob) {
		blob = drm_property_lookup_blob(plane->dev, value);
		if (!blob)
			return -EINVAL;

		if (blob->length != sizeof(struct drm_tegra_plane_csc_blob)) {
			drm_property_blob_put(blob);
			return -EINVAL;
		}

		drm_property_blob_put(tegra_state->csc_blob);
		tegra_state->csc_blob = blob;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int tegra_plane_get_property(struct drm_plane *plane,
				    const struct drm_plane_state *state,
				    struct drm_property *property,
				    uint64_t *value)
{
	struct tegra_plane *tegra = to_tegra_plane(plane);
	const struct tegra_plane_state *tegra_state;

	tegra_state = to_const_tegra_plane_state(state);

	if (property == tegra->props.csc_blob)
		*value = tegra_state->csc_blob->base.id;
	else
		return -EINVAL;

	return 0;
}

const struct drm_plane_funcs tegra_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = tegra_plane_destroy,
	.reset = tegra_plane_reset,
	.atomic_duplicate_state = tegra_plane_atomic_duplicate_state,
	.atomic_destroy_state = tegra_plane_atomic_destroy_state,
	.atomic_set_property = tegra_plane_set_property,
	.atomic_get_property = tegra_plane_get_property,
};

static int tegra_dc_pin(struct tegra_dc *dc, struct tegra_plane_state *state)
{
	struct drm_device *drm = dev_get_drvdata(dc->client.host);
	struct tegra_drm *tegra = drm->dev_private;
	unsigned int i;
	int err;

	for (i = 0; i < state->base.fb->format->num_planes; i++) {
		struct tegra_bo *bo = tegra_fb_get_plane(state->base.fb, i);

		err = tegra_drm_gart_map_optional(tegra, bo);
		if (err < 0)
			goto unpin;

		if (err > 0)
			state->iova[i] = bo->gartaddr;
		else
			state->iova[i] = bo->dmaaddr;
	}

	return 0;

unpin:
	dev_err(dc->dev, "failed to map plane %u: %d\n", i, err);

	while (i--) {
		struct tegra_bo *bo = tegra_fb_get_plane(state->base.fb, i);

		tegra_drm_gart_unmap_optional(tegra, bo);
	}

	return err;
}

static void tegra_dc_unpin(struct tegra_dc *dc, struct tegra_plane_state *state)
{
	struct drm_device *drm = dev_get_drvdata(dc->client.host);
	struct tegra_drm *tegra = drm->dev_private;
	unsigned int i;

	for (i = 0; i < state->base.fb->format->num_planes; i++) {
		struct tegra_bo *bo = tegra_fb_get_plane(state->base.fb, i);

		tegra_drm_gart_unmap_optional(tegra, bo);
	}
}

int tegra_plane_prepare_fb(struct drm_plane *plane,
			   struct drm_plane_state *state)
{
	struct tegra_dc *dc = to_tegra_dc(state->crtc);

	if (!state->fb)
		return 0;

	drm_gem_fb_prepare_fb(plane, state);

	return tegra_dc_pin(dc, to_tegra_plane_state(state));
}

void tegra_plane_cleanup_fb(struct drm_plane *plane,
			    struct drm_plane_state *state)
{
	struct tegra_dc *dc = to_tegra_dc(state->crtc);

	if (dc)
		tegra_dc_unpin(dc, to_tegra_plane_state(state));
}

static int tegra_plane_check_memory_bandwidth(struct drm_plane_state *state)
{
	struct tegra_plane_state *tegra_state = to_tegra_plane_state(state);
	unsigned int i, bpp, bpp_plane, dst_w, src_w, src_h, mul;
	const struct tegra_dc_soc_info *soc;
	const struct drm_format_info *fmt;
	struct drm_crtc_state *crtc_state;
	u32 avg_bandwidth, peak_bandwidth;

	if (!state->visible)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state->state, state->crtc);
	if (!crtc_state)
		return -EINVAL;

	src_w = drm_rect_width(&state->src) >> 16;
	src_h = drm_rect_height(&state->src) >> 16;
	dst_w = drm_rect_width(&state->dst);

	fmt = state->fb->format;
	soc = to_tegra_dc(state->crtc)->soc;

	/*
	 * Note that real memory bandwidth vary depending on format and
	 * memory layout, we are not taking that into account because small
	 * estimation error isn't important since bandwidth is rounded up
	 * anyway.
	 */
	for (i = 0, bpp = 0; i < fmt->num_planes; i++) {
		bpp_plane = fmt->cpp[i] * 8;

		/*
		 * Sub-sampling is relevant for chroma planes only and vertical
		 * readouts are not cached, hence only horizontal sub-sampling
		 * matters.
		 */
		if (i > 0)
			bpp_plane /= fmt->hsub;

		bpp += bpp_plane;
	}

	/*
	 * Horizontal downscale takes extra bandwidth which roughly depends
	 * on the scaled width.
	 */
	if (src_w > dst_w)
		mul = (src_w - dst_w) * bpp / 2048 + 1;
	else
		mul = 1;

	/* average bandwidth in bytes/s */
	avg_bandwidth  = src_w * src_h * bpp / 8 * mul;
	avg_bandwidth *= drm_mode_vrefresh(&crtc_state->mode);

	/* mode.clock in kHz, peak bandwidth in kbit/s */
	peak_bandwidth = crtc_state->mode.clock * bpp * mul;

	/* ICC bandwidth in kbyte/s */
	peak_bandwidth = kbps_to_icc(peak_bandwidth);
	avg_bandwidth  = Bps_to_icc(avg_bandwidth);

	/*
	 * Tegra30/114 Memory Controller can't interleave DC memory requests
	 * and DC uses 16-bytes atom for the tiled windows, while DDR3 uses 32
	 * bytes atom. Hence there is x2 memory overfetch for tiled framebuffer
	 * and DDR3 on older SoCs.
	 */
	if (soc->plane_tiled_memory_bandwidth_x2 &&
	    tegra_state->tiling.mode == TEGRA_BO_TILING_MODE_TILED) {
		peak_bandwidth *= 2;
		avg_bandwidth *= 2;
	}

	tegra_state->peak_memory_bandwidth = peak_bandwidth;
	tegra_state->avg_memory_bandwidth = avg_bandwidth;

	return 0;
}

int tegra_plane_state_add(struct tegra_plane *plane,
			  struct drm_plane_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct tegra_dc_state *tegra;
	int err;

	/* Propagate errors from allocation or locking failures. */
	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	/* Check plane state for visibility and calculate clipping bounds */
	err = drm_atomic_helper_check_plane_state(state, crtc_state,
						  0, INT_MAX, true, true);
	if (err < 0)
		return err;

	err = tegra_plane_check_memory_bandwidth(state);
	if (err < 0)
		return err;

	tegra = to_dc_state(crtc_state);

	tegra->planes |= WIN_A_ACT_REQ << plane->index;

	return 0;
}

int tegra_plane_format(u32 fourcc, u32 *format, u32 *swap)
{
	/* assume no swapping of fetched data */
	if (swap)
		*swap = BYTE_SWAP_NOSWAP;

	switch (fourcc) {
	case DRM_FORMAT_ARGB4444:
		*format = WIN_COLOR_DEPTH_B4G4R4A4;
		break;

	case DRM_FORMAT_ARGB1555:
		*format = WIN_COLOR_DEPTH_B5G5R5A1;
		break;

	case DRM_FORMAT_RGB565:
		*format = WIN_COLOR_DEPTH_B5G6R5;
		break;

	case DRM_FORMAT_RGBA5551:
		*format = WIN_COLOR_DEPTH_A1B5G5R5;
		break;

	case DRM_FORMAT_ARGB8888:
		*format = WIN_COLOR_DEPTH_B8G8R8A8;
		break;

	case DRM_FORMAT_ABGR8888:
		*format = WIN_COLOR_DEPTH_R8G8B8A8;
		break;

	case DRM_FORMAT_ABGR4444:
		*format = WIN_COLOR_DEPTH_R4G4B4A4;
		break;

	case DRM_FORMAT_ABGR1555:
		*format = WIN_COLOR_DEPTH_R5G5B5A;
		break;

	case DRM_FORMAT_BGRA5551:
		*format = WIN_COLOR_DEPTH_AR5G5B5;
		break;

	case DRM_FORMAT_XRGB1555:
		*format = WIN_COLOR_DEPTH_B5G5R5X1;
		break;

	case DRM_FORMAT_RGBX5551:
		*format = WIN_COLOR_DEPTH_X1B5G5R5;
		break;

	case DRM_FORMAT_XBGR1555:
		*format = WIN_COLOR_DEPTH_R5G5B5X1;
		break;

	case DRM_FORMAT_BGRX5551:
		*format = WIN_COLOR_DEPTH_X1R5G5B5;
		break;

	case DRM_FORMAT_BGR565:
		*format = WIN_COLOR_DEPTH_R5G6B5;
		break;

	case DRM_FORMAT_BGRA8888:
		*format = WIN_COLOR_DEPTH_A8R8G8B8;
		break;

	case DRM_FORMAT_RGBA8888:
		*format = WIN_COLOR_DEPTH_A8B8G8R8;
		break;

	case DRM_FORMAT_XRGB8888:
		*format = WIN_COLOR_DEPTH_B8G8R8X8;
		break;

	case DRM_FORMAT_XBGR8888:
		*format = WIN_COLOR_DEPTH_R8G8B8X8;
		break;

	case DRM_FORMAT_UYVY:
		*format = WIN_COLOR_DEPTH_YCbCr422;
		break;

	case DRM_FORMAT_YUYV:
		if (!swap)
			return -EINVAL;

		*format = WIN_COLOR_DEPTH_YCbCr422;
		*swap = BYTE_SWAP_SWAP2;
		break;

	case DRM_FORMAT_YUV420:
		*format = WIN_COLOR_DEPTH_YCbCr420P;
		break;

	case DRM_FORMAT_YUV422:
		*format = WIN_COLOR_DEPTH_YCbCr422P;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

bool tegra_plane_format_is_yuv(unsigned int format, bool *planar)
{
	switch (format) {
	case WIN_COLOR_DEPTH_YCbCr422:
	case WIN_COLOR_DEPTH_YUV422:
		if (planar)
			*planar = false;

		return true;

	case WIN_COLOR_DEPTH_YCbCr420P:
	case WIN_COLOR_DEPTH_YUV420P:
	case WIN_COLOR_DEPTH_YCbCr422P:
	case WIN_COLOR_DEPTH_YUV422P:
	case WIN_COLOR_DEPTH_YCbCr422R:
	case WIN_COLOR_DEPTH_YUV422R:
	case WIN_COLOR_DEPTH_YCbCr422RA:
	case WIN_COLOR_DEPTH_YUV422RA:
		if (planar)
			*planar = true;

		return true;
	}

	if (planar)
		*planar = false;

	return false;
}

static bool __drm_format_has_alpha(u32 format)
{
	switch (format) {
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_ARGB8888:
		return true;
	}

	return false;
}

static int tegra_plane_format_get_alpha(unsigned int opaque,
					unsigned int *alpha)
{
	if (tegra_plane_format_is_yuv(opaque, NULL)) {
		*alpha = opaque;
		return 0;
	}

	switch (opaque) {
	case WIN_COLOR_DEPTH_B5G5R5X1:
		*alpha = WIN_COLOR_DEPTH_B5G5R5A1;
		return 0;

	case WIN_COLOR_DEPTH_X1B5G5R5:
		*alpha = WIN_COLOR_DEPTH_A1B5G5R5;
		return 0;

	case WIN_COLOR_DEPTH_R8G8B8X8:
		*alpha = WIN_COLOR_DEPTH_R8G8B8A8;
		return 0;

	case WIN_COLOR_DEPTH_B8G8R8X8:
		*alpha = WIN_COLOR_DEPTH_B8G8R8A8;
		return 0;

	case WIN_COLOR_DEPTH_B5G6R5:
		*alpha = opaque;
		return 0;
	}

	return -EINVAL;
}

/*
 * This is applicable to Tegra20 and Tegra30 only where the opaque formats can
 * be emulated using the alpha formats and alpha blending disabled.
 */
static int tegra_plane_setup_opacity(struct tegra_plane *tegra,
				     struct tegra_plane_state *state)
{
	unsigned int format;
	int err;

	switch (state->format) {
	case WIN_COLOR_DEPTH_B5G5R5A1:
	case WIN_COLOR_DEPTH_A1B5G5R5:
	case WIN_COLOR_DEPTH_R8G8B8A8:
	case WIN_COLOR_DEPTH_B8G8R8A8:
		state->opaque = false;
		break;

	default:
		err = tegra_plane_format_get_alpha(state->format, &format);
		if (err < 0)
			return err;

		state->format = format;
		state->opaque = true;
		break;
	}

	return 0;
}

static int tegra_plane_check_transparency(struct tegra_plane *tegra,
					  struct tegra_plane_state *state)
{
	struct drm_plane_state *old, *plane_state;
	struct drm_plane *plane;

	old = drm_atomic_get_old_plane_state(state->base.state, &tegra->base);

	/* check if zpos / transparency changed */
	if (old->normalized_zpos == state->base.normalized_zpos &&
	    to_tegra_plane_state(old)->opaque == state->opaque)
		return 0;

	/* include all sibling planes into this commit */
	drm_for_each_plane(plane, tegra->base.dev) {
		struct tegra_plane *p = to_tegra_plane(plane);

		/* skip this plane and planes on different CRTCs */
		if (p == tegra || p->dc != tegra->dc)
			continue;

		plane_state = drm_atomic_get_plane_state(state->base.state,
							 plane);
		if (IS_ERR(plane_state))
			return PTR_ERR(plane_state);
	}

	return 1;
}

static unsigned int tegra_plane_get_overlap_index(struct tegra_plane *plane,
						  struct tegra_plane *other)
{
	unsigned int index = 0, i;

	WARN_ON(plane == other);

	for (i = 0; i < 3; i++) {
		if (i == plane->index)
			continue;

		if (i == other->index)
			break;

		index++;
	}

	return index;
}

static void tegra_plane_update_transparency(struct tegra_plane *tegra,
					    struct tegra_plane_state *state)
{
	struct drm_plane_state *new;
	struct drm_plane *plane;
	unsigned int i;

	for_each_new_plane_in_state(state->base.state, plane, new, i) {
		struct tegra_plane *p = to_tegra_plane(plane);
		unsigned index;

		/* skip this plane and planes on different CRTCs */
		if (p == tegra || p->dc != tegra->dc)
			continue;

		index = tegra_plane_get_overlap_index(tegra, p);

		if (new->fb && __drm_format_has_alpha(new->fb->format->format))
			state->blending[index].alpha = true;
		else
			state->blending[index].alpha = false;

		if (new->normalized_zpos > state->base.normalized_zpos)
			state->blending[index].top = true;
		else
			state->blending[index].top = false;

		/*
		 * Missing framebuffer means that plane is disabled, in this
		 * case mark B / C window as top to be able to differentiate
		 * windows indices order in regards to zPos for the middle
		 * window X / Y registers programming.
		 */
		if (!new->fb)
			state->blending[index].top = (index == 1);
	}
}

static int tegra_plane_setup_transparency(struct tegra_plane *tegra,
					  struct tegra_plane_state *state)
{
	struct tegra_plane_state *tegra_state;
	struct drm_plane_state *new;
	struct drm_plane *plane;
	int err;

	/*
	 * If planes zpos / transparency changed, sibling planes blending
	 * state may require adjustment and in this case they will be included
	 * into this atom commit, otherwise blending state is unchanged.
	 */
	err = tegra_plane_check_transparency(tegra, state);
	if (err <= 0)
		return err;

	/*
	 * All planes are now in the atomic state, walk them up and update
	 * transparency state for each plane.
	 */
	drm_for_each_plane(plane, tegra->base.dev) {
		struct tegra_plane *p = to_tegra_plane(plane);

		/* skip planes on different CRTCs */
		if (p->dc != tegra->dc)
			continue;

		new = drm_atomic_get_new_plane_state(state->base.state, plane);
		tegra_state = to_tegra_plane_state(new);

		/*
		 * There is no need to update blending state for the disabled
		 * plane.
		 */
		if (new->fb)
			tegra_plane_update_transparency(p, tegra_state);
	}

	return 0;
}

static u32 tegra_plane_colorkey_to_hw_format(u64 drm_ckey64)
{
	/* convert ARGB16161616 to ARGB8888 */
	u8 a = drm_colorkey_extract_component(drm_ckey64, alpha, 8);
	u8 r = drm_colorkey_extract_component(drm_ckey64, red, 8);
	u8 g = drm_colorkey_extract_component(drm_ckey64, green, 8);
	u8 b = drm_colorkey_extract_component(drm_ckey64, blue, 8);

	return (a << 24) | (r << 16) | (g << 8) | b;
}

static bool tegra_plane_format_valid_for_colorkey(struct drm_plane_state *state)
{
	struct tegra_plane_state *tegra_state = to_tegra_plane_state(state);

	/*
	 * Tegra20 does not support alpha channel matching. Newer Tegra's
	 * support the alpha matching, but it is not implemented yet.
	 *
	 * Formats other than XRGB8888 haven't been tested much, hence they
	 * are not supported for now.
	 */
	switch (tegra_state->format) {
	case WIN_COLOR_DEPTH_R8G8B8X8:
	case WIN_COLOR_DEPTH_B8G8R8X8:
		break;

	default:
		return false;
	};

	return true;
}

static int tegra_plane_setup_colorkey(struct tegra_plane *tegra,
				      struct tegra_plane_state *tegra_state)
{
	enum drm_plane_colorkey_mode mode;
	struct drm_crtc_state *crtc_state;
	struct tegra_dc_state *dc_state;
	struct drm_plane_state *state;
	struct drm_plane_state *old;
	struct drm_plane_state *new;
	struct drm_plane *plane;
	unsigned int normalized_zpos;
	u32 min_hw, max_hw, mask_hw;
	u32 plane_mask;
	u64 min, max;
	u64 mask;

	normalized_zpos = tegra_state->base.normalized_zpos;
	plane_mask = tegra_state->base.colorkey.plane_mask;
	mode = tegra_state->base.colorkey.mode;
	mask = tegra_state->base.colorkey.mask;
	min = tegra_state->base.colorkey.min;
	max = tegra_state->base.colorkey.max;

	/* convert color key values to HW format */
	mask_hw = tegra_plane_colorkey_to_hw_format(mask);
	min_hw = tegra_plane_colorkey_to_hw_format(min);
	max_hw = tegra_plane_colorkey_to_hw_format(max);

	state = &tegra_state->base;
	old = drm_atomic_get_old_plane_state(state->state, &tegra->base);

	/* no need to proceed if color keying state is unchanged */
	if (old->colorkey.plane_mask == plane_mask &&
	    old->colorkey.mask == mask &&
	    old->colorkey.mode == mode &&
	    old->colorkey.min == min &&
	    old->colorkey.max == max &&
	    old->crtc)
	{
		if (mode == DRM_PLANE_COLORKEY_MODE_DISABLED)
			return 0;

		crtc_state = drm_atomic_get_crtc_state(state->state,
						       state->crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		if (!crtc_state->zpos_changed) {
			dc_state = to_dc_state(crtc_state);

			if (dc_state->ckey.min == min_hw &&
			    dc_state->ckey.max == max_hw)
				return 0;
		}
	}

	/*
	 * Currently color keying is implemented for the middle plane
	 * only (source and destination) to simplify things, validate planes
	 * position and mask.
	 */
	if (state->fb && mode != DRM_PLANE_COLORKEY_MODE_DISABLED) {
		/*
		 * Tegra does not support color key masking, note that alpha
		 * channel mask is ignored because only opaque formats are
		 * currently supported.
		 */
		if ((mask_hw & 0xffffff) != 0xffffff)
			return -EINVAL;

		drm_for_each_plane_mask(plane, tegra->base.dev, plane_mask) {
			struct tegra_plane *p = to_tegra_plane(plane);

			/* HW can't access planes on a different CRTC */
			if (p->dc != tegra->dc)
				return -EINVAL;

			new = drm_atomic_get_plane_state(state->state, plane);
			if (IS_ERR(new))
				return PTR_ERR(new);

			/* don't care about disabled plane */
			if (!new->fb)
				continue;

			if (!tegra_plane_format_valid_for_colorkey(new))
				return -EINVAL;

			/* middle plane sourcing itself */
			if (new->normalized_zpos == 1 &&
			    normalized_zpos == 1)
				continue;

			return -EINVAL;
		}
	}

	/* only middle plane affects the color key state, see comment above */
	if (normalized_zpos != 1)
		return 0;

	/*
	 * Tegra's HW has color key values stored within CRTC, hence adjust
	 * planes CRTC atomic state.
	 */
	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	dc_state = to_dc_state(crtc_state);

	/* update CRTC's color key state */
	dc_state->ckey.min = min_hw;
	dc_state->ckey.max = max_hw;

	return 0;
}

int tegra_plane_setup_legacy_state(struct tegra_plane *tegra,
				   struct tegra_plane_state *state)
{
	int err;

	err = tegra_plane_setup_opacity(tegra, state);
	if (err < 0)
		return err;

	err = tegra_plane_setup_transparency(tegra, state);
	if (err < 0)
		return err;

	err = tegra_plane_setup_colorkey(tegra, state);
	if (err < 0)
		return err;

	return 0;
}

static const char * const tegra_plane_icc_names[] = {
	"wina", "winb", "winc", "", "", "", "cursor",
};

int tegra_plane_interconnect_init(struct tegra_plane *plane)
{
	const char *icc_name = tegra_plane_icc_names[plane->index];
	struct device *dev = plane->dc->dev;
	struct tegra_dc *dc = plane->dc;
	int err;

	plane->icc_mem = devm_of_icc_get(dev, icc_name);
	err = PTR_ERR_OR_ZERO(plane->icc_mem);
	if (err) {
		dev_err_probe(dev, err, "failed to get %s interconnect\n",
			      icc_name);
		return err;
	}

	/* plane B on T20/30 has a dedicated memory client for a 6-tap vertical filter */
	if (plane->index == 1 && dc->soc->has_win_b_vfilter_mem_client) {
		plane->icc_mem_vfilter = devm_of_icc_get(dev, "winb-vfilter");
		err = PTR_ERR_OR_ZERO(plane->icc_mem_vfilter);
		if (err) {
			dev_err_probe(dev, err, "failed to get %s interconnect\n",
				      "winb-vfilter");
			return err;
		}
	}

	return 0;
}

void tegra_plane_copy_state(struct drm_plane *plane,
			    struct drm_plane_state *state)
{
	struct tegra_plane_state *tegra = to_tegra_plane_state(plane->state);
	struct tegra_plane_state *tegra_new = to_tegra_plane_state(state);
	unsigned int i;

	swap(plane->state->fb, state->fb);
	plane->state->crtc_x = state->crtc_x;
	plane->state->crtc_y = state->crtc_y;
	plane->state->crtc_w = state->crtc_w;
	plane->state->crtc_h = state->crtc_h;
	plane->state->src_x = state->src_x;
	plane->state->src_y = state->src_y;
	plane->state->src_w = state->src_w;
	plane->state->src_h = state->src_h;
	plane->state->alpha = state->alpha;
	plane->state->rotation = state->rotation;
	plane->state->zpos = state->zpos;
	plane->state->normalized_zpos = state->normalized_zpos;
	plane->state->src = state->src;
	plane->state->dst = state->dst;
	plane->state->visible = state->visible;

	tegra->swap = tegra_new->swap;
	tegra->tiling = tegra_new->tiling;
	tegra->format = tegra_new->format;
	tegra->opaque = tegra_new->opaque;
	tegra->reflect_x = tegra_new->reflect_x;
	tegra->reflect_y = tegra_new->reflect_y;
	tegra->avg_memory_bandwidth = tegra_new->avg_memory_bandwidth;
	tegra->peak_memory_bandwidth = tegra_new->peak_memory_bandwidth;

	for (i = 0; i < 2; i++)
		tegra->blending[i] = tegra_new->blending[i];

	if (tegra->csc_blob != tegra_new->csc_blob) {
		drm_property_blob_put(tegra->csc_blob);

		tegra->csc_blob = drm_property_blob_get(tegra_new->csc_blob);
	}
}
