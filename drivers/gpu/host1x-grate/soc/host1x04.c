/*
 * Host1x init for Tegra124 SoCs
 *
 * Copyright (c) 2013 NVIDIA Corporation.
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

#include "host1x.h"
#include "host1x04.h"

/* include hardware specification */
#include "hw/host1x04_hardware.h"

#define HOST1X_HW		4
#define HOST1X_SYNCPTS_NUM	192
#define HOST1X_CHANNELS_NUM	12
#define HOST1X_SYNC_OFFSET	0x2100
#define HOST1X_MLOCKS_NUM	16

/* include code */
#include "debug.c"
#include "pushbuf.c"
#include "channel_hw.c"
#include "syncpoints_hw.c"
#include "channel.c"
#include "syncpoints.c"
#include "mlocks_hw.c"
#include "mlocks.c"

int host1x04_init(struct host1x *host)
{
	host->chan_ops.init		= host1x_soc_init_channels;
	host->chan_ops.deinit		= host1x_soc_deinit_channels;
	host->chan_ops.request		= host1x_soc_channel_request;
	host->chan_ops.release		= host1x_soc_release_channel;
	host->chan_ops.reset		= host1x_soc_channel_reset;
	host->chan_ops.reinit		= host1x_soc_channel_reinit;
	host->chan_ops.stop		= host1x_soc_channel_stop;
	host->chan_ops.submit		= host1x_soc_channel_submit;
	host->chan_ops.cleanup_job	= host1x_soc_channel_cleanup_job;
	host->chan_ops.dmaget		= host1x_soc_channel_dmaget;

	host->syncpt_ops.init		= host1x_soc_init_syncpts;
	host->syncpt_ops.deinit		= host1x_soc_deinit_syncpts;
	host->syncpt_ops.reinit		= host1x_soc_reinit_syncpts;
	host->syncpt_ops.request	= host1x_soc_syncpt_request;
	host->syncpt_ops.release	= host1x_soc_syncpt_release;
	host->syncpt_ops.reset		= host1x_soc_syncpt_reset;
	host->syncpt_ops.set_interrupt	= host1x_soc_syncpt_set_interrupt;
	host->syncpt_ops.read		= host1x_soc_syncpt_read;
	host->syncpt_ops.detach_fences	= host1x_soc_syncpt_detach_fences;

	host->mlock_ops.init		= host1x_soc_init_mlocks;
	host->mlock_ops.deinit		= host1x_soc_deinit_mlocks;
	host->mlock_ops.request		= host1x_soc_mlock_request;
	host->mlock_ops.release		= host1x_soc_mlock_release;
	host->mlock_ops.unlock_channel	= host1x_soc_mlock_unlock_channel;

	host->dbg_ops.dump_cmdbuf	= host1x_soc_dump_cmdbuf;
	host->dbg_ops.dump_syncpt	= host1x_soc_dump_syncpt;
	host->dbg_ops.dump_syncpts	= host1x_soc_dump_syncpts;
	host->dbg_ops.dump_channel	= host1x_soc_dump_channel;
	host->dbg_ops.dump_channels	= host1x_soc_dump_channels;
	host->dbg_ops.dump_mlocks	= host1x_soc_dump_mlocks;

	return 0;
}
