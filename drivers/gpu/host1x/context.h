/*
 * Copyright 2017 Dmitry Osipenko <digetx@gmail.com>
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

#ifndef __HOST1X_CONTEXT_H
#define __HOST1X_CONTEXT_H

#include <linux/kref.h>
#include <linux/host1x.h>

#include "intr.h"

struct host1x_context {
	const struct host1x_context_ops *ops;
	struct host1x_channel *channel;
	struct host1x_client *client;
	struct host1x_syncpt *sp;
	enum host1x_class class;
	struct kref ref;

	struct host1x_bo *bo;
	struct sg_table *sgt;

	struct host1x_context_push_data *restore_data;
	struct host1x_context_push_data *store_data;
	unsigned int restore_pushes;
	unsigned int store_pushes;
	unsigned int words_num;

	phys_addr_t bo_phys;
	dma_addr_t bo_dma;
	void *bo_vaddr;
	u32 bo_offset;

	bool hw_store;
	bool sw_store;
	bool inited;
};

int host1x_context_schedule_dma_tx(struct host1x_context *ctx);
void host1x_context_store(struct host1x_context *ctx);
bool host1x_context_store_required(struct host1x_context *ctx);
bool host1x_context_restore_required(struct host1x_context *ctx);
void host1x_context_get_recent(struct host1x_channel *ch);
void host1x_context_update_recent(struct host1x_channel *channel,
				  struct host1x_context *ctx,
				  bool release);

#endif
