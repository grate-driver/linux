/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/io.h>

#if HOST1X_HW < 6

#define MLOCK(idx)							\
	(host->base_regs + HOST1X_SYNC_OFFSET +				\
		HOST1X_SYNC_MLOCK(idx))

#define MLOCK_OWNER(idx)						\
	(host->base_regs + HOST1X_SYNC_OFFSET +				\
		HOST1X_SYNC_MLOCK_OWNER(idx))

static inline u32
host1x_hw_mlock_owner(struct host1x *host, unsigned int id)
{
	return readl_relaxed(MLOCK_OWNER(id));
}

static inline void
host1x_hw_mlock_unlock(struct host1x *host, unsigned int id)
{
	writel_relaxed(0, MLOCK(id));
}

#endif
