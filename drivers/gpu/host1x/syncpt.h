/*
 * Tegra host1x Syncpoints
 *
 * Copyright (c) 2010-2013, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __HOST1X_SYNCPT_H
#define __HOST1X_SYNCPT_H

#include <linux/atomic.h>
#include <linux/host1x.h>
#include <linux/kernel.h>
#include <linux/kref.h>

#include "intr.h"

struct host1x;

struct host1x_syncpt_base {
	unsigned int id;
	u32 value;
};

struct host1x_syncpt {
	unsigned int id;
	atomic_t min_val;
	atomic_t max_val;
	const char *name;
	bool client_managed;
	struct host1x *host;
	struct host1x_syncpt_base *base;

	/* interrupt data */
	struct host1x_syncpt_intr intr;

	struct kref refcount;
};

/* Initialize sync point array  */
int host1x_syncpt_init(struct host1x *host);

/*  Free sync point array */
void host1x_syncpt_deinit(struct host1x *host);

/* Return number of sync point supported. */
unsigned int host1x_syncpt_nb_pts(struct host1x *host);

/* Return number of wait bases supported. */
unsigned int host1x_syncpt_nb_bases(struct host1x *host);

/* Return number of mlocks supported. */
unsigned int host1x_syncpt_nb_mlocks(struct host1x *host);

/* Load current value from hardware to the shadow register. */
u32 host1x_syncpt_load(struct host1x_syncpt *sp);

/* Check if the given syncpoint value has already passed */
bool host1x_syncpt_is_expired(struct host1x_syncpt *sp, u32 thresh);

/* Save host1x sync point state into shadow registers. */
void host1x_syncpt_save(struct host1x *host);

/* Reset host1x sync point state from shadow registers. */
void host1x_syncpt_restore(struct host1x *host);

/* Read current wait base value into shadow register and return it. */
u32 host1x_syncpt_load_wait_base(struct host1x_syncpt *sp);

/* Indicate future operations by incrementing the sync point max. */
u32 host1x_syncpt_incr_max(struct host1x_syncpt *sp, u32 incrs);

#endif
