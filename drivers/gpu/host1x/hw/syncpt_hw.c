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

#include <linux/io.h>

#include "../dev.h"
#include "../syncpt.h"

/*
 * Write the syncpoint value to hw.
 */
static void syncpt_restore(struct host1x *host, u32 syncpt_id, u32 value)
{
	host1x_sync_writel(host, value, HOST1X_SYNC_SYNCPT(syncpt_id));
}

/*
 * Write the waitbase value to hw.
 */
static void syncpt_restore_wait_base(struct host1x *host,
				     u32 base_id, u32 value)
{
	host1x_sync_writel(host, value, HOST1X_SYNC_SYNCPT_BASE(base_id));
}

/*
 * Read waitbase value from hw.
 */
static u32 syncpt_read_wait_base(struct host1x *host, u32 base_id)
{
	return host1x_sync_readl(host, HOST1X_SYNC_SYNCPT_BASE(base_id));
}

/*
 * Read syncpoint value from hw.
 */
static u32 syncpt_load(struct host1x *host, u32 syncpt_id)
{
	return host1x_sync_readl(host, HOST1X_SYNC_SYNCPT(syncpt_id));
}

/*
 * Write a cpu syncpoint increment to the hardware, without touching
 * the cache.
 */
static void syncpt_cpu_incr(struct host1x *host, u32 syncpt_id)
{
	host1x_sync_writel(host, BIT(syncpt_id % 32),
			   HOST1X_SYNC_SYNCPT_CPU_INCR(syncpt_id / 32));
}

/* remove a wait pointed to by patch_addr */
static int syncpt_patch_wait(struct host1x_syncpt *sp, void *patch_addr)
{
	u32 override = host1x_class_host_wait_syncpt(HOST1X_SYNCPT_RESERVED, 0);

	*((u32 *)patch_addr) = override;

	return 0;
}

static const struct host1x_syncpt_ops host1x_syncpt_ops = {
	.restore = syncpt_restore,
	.restore_wait_base = syncpt_restore_wait_base,
	.load_wait_base = syncpt_read_wait_base,
	.load = syncpt_load,
	.cpu_incr = syncpt_cpu_incr,
	.patch_wait = syncpt_patch_wait,
};
