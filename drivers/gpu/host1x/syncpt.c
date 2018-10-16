// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tegra host1x Syncpoints
 *
 * Copyright (c) 2010-2015, NVIDIA Corporation.
 */

#include "dev.h"

struct host1x_syncpt *host1x_syncpt_get(struct host1x *host, u32 id)
{
	return NULL;
}

u32 host1x_syncpt_id(struct host1x_syncpt *sp)
{
	return 0;
}

u32 host1x_syncpt_read_min(struct host1x_syncpt *sp)
{
	return 0;
}

u32 host1x_syncpt_read_max(struct host1x_syncpt *sp)
{
	return 0;
}

u32 host1x_syncpt_read(struct host1x_syncpt *sp)
{
	return 0;
}

int host1x_syncpt_incr(struct host1x_syncpt *sp)
{
	return 0;
}

u32 host1x_syncpt_incr_max(struct host1x_syncpt *sp, u32 incrs)
{
	return 0;
}

int host1x_syncpt_wait(struct host1x_syncpt *sp, u32 thresh, long timeout,
		       u32 *value)
{
	return 0;
}

struct host1x_syncpt *host1x_syncpt_request(struct host1x_client *client,
					    unsigned long flags)
{
	return NULL;
}

void host1x_syncpt_free(struct host1x_syncpt *sp)
{
}

struct host1x_syncpt_base *host1x_syncpt_get_base(struct host1x_syncpt *sp)
{
	return NULL;
}

u32 host1x_syncpt_base_id(struct host1x_syncpt_base *base)
{
	return 0;
}
