/*
 * Copyright (C) 2017 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_plane_helper.h>

#include "dc.h"
#include "plane.h"

static void tegra_plane_destroy(struct drm_plane *plane)
{
	struct tegra_plane *p = to_tegra_plane(plane);

	drm_plane_cleanup(plane);
	kfree(p);
}

static void tegra_plane_reset(struct drm_plane *plane)
{
	struct tegra_plane_state *state;

	if (plane->state)
		__drm_atomic_helper_plane_destroy_state(plane->state);

	kfree(plane->state);
	plane->state = NULL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state) {
		plane->state = &state->base;
		plane->state->plane = plane;
	}
}

static struct drm_plane_state *
tegra_plane_atomic_duplicate_state(struct drm_plane *plane)
{
	struct tegra_plane_state *state = to_tegra_plane_state(plane->state);
	struct tegra_plane_state *copy;

	copy = kmalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &copy->base);
	copy->tiling = state->tiling;
	copy->format = state->format;
	copy->swap = state->swap;

	return &copy->base;
}

static void tegra_plane_atomic_destroy_state(struct drm_plane *plane,
					     struct drm_plane_state *state)
{
	__drm_atomic_helper_plane_destroy_state(state);
	kfree(state);
}

const struct drm_plane_funcs tegra_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = tegra_plane_destroy,
	.reset = tegra_plane_reset,
	.atomic_duplicate_state = tegra_plane_atomic_duplicate_state,
	.atomic_destroy_state = tegra_plane_atomic_destroy_state,
};

int tegra_plane_state_add(struct tegra_plane *plane,
			  struct drm_plane_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct tegra_dc_state *tegra;
	struct drm_rect clip;
	int err;

	/* Propagate errors from allocation or locking failures. */
	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	clip.x1 = 0;
	clip.y1 = 0;
	clip.x2 = crtc_state->mode.hdisplay;
	clip.y2 = crtc_state->mode.vdisplay;

	/* Check plane state for visibility and calculate clipping bounds */
	err = drm_atomic_helper_check_plane_state(state, crtc_state, &clip,
						  0, INT_MAX, true, true);
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

/*
 * This is applicable to Tegra20 and Tegra30 only where the opaque formats can
 * be emulated using the alpha formats and alpha blending disabled.
 */
unsigned int tegra_plane_format_adjust(unsigned int opaque)
{
	switch (opaque) {
	case WIN_COLOR_DEPTH_B5G5R5X1:
		return WIN_COLOR_DEPTH_B5G5R5A1;

	case WIN_COLOR_DEPTH_X1B5G5R5:
		return WIN_COLOR_DEPTH_A1B5G5R5;

	case WIN_COLOR_DEPTH_R8G8B8X8:
		return WIN_COLOR_DEPTH_R8G8B8A8;

	case WIN_COLOR_DEPTH_B8G8R8X8:
		return WIN_COLOR_DEPTH_B8G8R8A8;
	}

	return opaque;
}

int tegra_plane_update_blending_state(struct tegra_plane *tegra,
				      struct tegra_plane_state *state)
{
	u32 blend_transparent = BLEND_WEIGHT1(0) | BLEND_WEIGHT0(0);
	u32 blend_opaque = BLEND_WEIGHT1(255) | BLEND_WEIGHT0(255);
	struct tegra_dc_blend_state *blend_state;
	struct tegra_dc_blend_state *win_a_state;
	struct tegra_dc_blend_state *win_b_state;
	struct tegra_dc_blend_state *win_c_state;
	struct tegra_dc_state *dc_state;
	struct drm_crtc_state *crtc_state;

	crtc_state = drm_atomic_get_crtc_state(state->base.state,
					       state->base.crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	dc_state = to_dc_state(crtc_state);
	blend_state = &dc_state->blend[tegra->index];
	blend_state->opaque = !__drm_format_has_alpha(
					state->base.fb->format->format);

	win_a_state = &dc_state->blend[0];
	win_b_state = &dc_state->blend[1];
	win_c_state = &dc_state->blend[2];

	/* setup blending state for window A */

	if (win_b_state->opaque) {
		win_a_state->to_win_x = blend_transparent;
	} else {
		if (win_a_state->opaque)
			win_a_state->to_win_x = BLEND_CONTROL_DEPENDENT;
		else
			win_a_state->to_win_x = BLEND_CONTROL_ALPHA;
	}

	if (win_c_state->opaque) {
		win_a_state->to_win_y = blend_transparent;
	} else {
		if (win_a_state->opaque)
			win_a_state->to_win_y = BLEND_CONTROL_DEPENDENT;
		else
			win_a_state->to_win_y = BLEND_CONTROL_ALPHA;
	}

	if (win_b_state->opaque || win_c_state->opaque) {
		win_a_state->to_win_xy = blend_transparent;
	} else {
		if (win_a_state->opaque)
			win_a_state->to_win_xy = BLEND_CONTROL_DEPENDENT;
		else
			win_a_state->to_win_xy = BLEND_CONTROL_ALPHA;
	}

	/* setup blending state for window B */

	if (win_b_state->opaque)
		win_b_state->to_win_x = blend_opaque;
	else
		win_b_state->to_win_x = BLEND_CONTROL_ALPHA;

	if (win_c_state->opaque) {
		win_b_state->to_win_y = blend_transparent;
		win_b_state->to_win_xy = blend_transparent;
	} else {
		if (win_b_state->opaque) {
			win_b_state->to_win_y = BLEND_CONTROL_DEPENDENT;
			win_b_state->to_win_xy = BLEND_CONTROL_DEPENDENT;
		} else {
			win_b_state->to_win_y = BLEND_CONTROL_ALPHA;
			win_b_state->to_win_xy = BLEND_CONTROL_ALPHA;
		}
	}

	/* setup blending state for window C */

	if (win_c_state->opaque) {
		win_c_state->to_win_x = blend_opaque;
		win_c_state->to_win_y = blend_opaque;
		win_c_state->to_win_xy = blend_opaque;
	} else {
		win_c_state->to_win_x = BLEND_CONTROL_ALPHA;
		win_c_state->to_win_y = BLEND_CONTROL_ALPHA;
		win_c_state->to_win_xy = BLEND_CONTROL_ALPHA;
	}

	return 0;
}
