/*
 * Tegra host1x Channel
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

#include <linux/slab.h>
#include <linux/module.h>

#include "channel.h"
#include "dev.h"
#include "job.h"

#define INDDATA_FIFO_ADDR(ch)	(0x5000000c + (ch) * 0x4000)

/* Constructor for the host1x device list */
int host1x_channel_list_init(struct host1x_channel_list *chlist,
			     unsigned int num_channels)
{
	chlist->channels = kcalloc(num_channels, sizeof(struct host1x_channel),
				   GFP_KERNEL);
	if (!chlist->channels)
		return -ENOMEM;

	chlist->allocated_channels =
		kcalloc(BITS_TO_LONGS(num_channels), sizeof(unsigned long),
			GFP_KERNEL);
	if (!chlist->allocated_channels) {
		kfree(chlist->channels);
		return -ENOMEM;
	}

	return 0;
}

void host1x_channel_list_free(struct host1x_channel_list *chlist)
{
	kfree(chlist->allocated_channels);
	kfree(chlist->channels);
}

int host1x_job_submit(struct host1x_job *job)
{
	struct host1x *host = dev_get_drvdata(job->channel->dev->parent);

	return host1x_hw_channel_submit(host, job);
}
EXPORT_SYMBOL(host1x_job_submit);

struct host1x_channel *host1x_channel_get(struct host1x_channel *channel)
{
	kref_get(&channel->refcount);

	return channel;
}
EXPORT_SYMBOL(host1x_channel_get);

/**
 * host1x_channel_get_index() - Attempt to get channel reference by index
 * @host: Host1x device object
 * @index: Index of channel
 *
 * If channel number @index is currently allocated, increase its refcount
 * and return a pointer to it. Otherwise, return NULL.
 */
struct host1x_channel *host1x_channel_get_index(struct host1x *host,
						unsigned int index)
{
	struct host1x_channel *ch = &host->channel_list.channels[index];

	if (!kref_get_unless_zero(&ch->refcount))
		return NULL;

	return ch;
}

static void release_channel(struct kref *kref)
{
	struct host1x_channel *channel =
		container_of(kref, struct host1x_channel, refcount);
	struct host1x *host = dev_get_drvdata(channel->dev->parent);
	struct host1x_channel_list *chlist = &host->channel_list;

	host1x_hw_cdma_stop(host, &channel->cdma);
	host1x_cdma_deinit(&channel->cdma);

	clear_bit(channel->id, chlist->allocated_channels);
}

void host1x_channel_put(struct host1x_channel *channel)
{
	kref_put(&channel->refcount, release_channel);
}
EXPORT_SYMBOL(host1x_channel_put);

static struct host1x_channel *acquire_unused_channel(struct host1x *host)
{
	struct host1x_channel_list *chlist = &host->channel_list;
	unsigned int max_channels = host->info->nb_channels;
	unsigned int index;

	index = find_first_zero_bit(chlist->allocated_channels, max_channels);
	if (index >= max_channels) {
		dev_err(host->dev, "failed to find free channel\n");
		return NULL;
	}

	chlist->channels[index].id = index;

	set_bit(index, chlist->allocated_channels);

	return &chlist->channels[index];
}

/**
 * host1x_channel_request() - Allocate a channel
 * @device: Host1x unit this channel will be used to send commands to
 *
 * Allocates a new host1x channel for @device. May return NULL if CDMA
 * initialization fails.
 */
struct host1x_channel *host1x_channel_request(struct device *dev)
{
	struct host1x *host = dev_get_drvdata(dev->parent);
	struct host1x_channel_list *chlist = &host->channel_list;
	struct host1x_channel *channel;
	int err;

	channel = acquire_unused_channel(host);
	if (!channel)
		return NULL;

	kref_init(&channel->refcount);
	mutex_init(&channel->submitlock);
	spin_lock_init(&channel->context_lock);
	channel->dev = dev;

	err = host1x_hw_channel_init(host, channel, channel->id);
	if (err < 0)
		goto fail;

	err = host1x_cdma_init(&channel->cdma);
	if (err < 0)
		goto fail;

	/* enable HW firewall on Tegra124+ */
	host1x_hw_firewall_enable_gather_filter(host, channel);

	return channel;

fail:
	clear_bit(channel->id, chlist->allocated_channels);

	dev_err(dev, "failed to initialize channel\n");

	return NULL;
}
EXPORT_SYMBOL(host1x_channel_request);

/**
 * host1x_channel_enable_dma_flowctrl() - Setup DRQ to AHB DMA
 * @channel: Host1x channel that will request DMA to read data
 *
 * Enables channel-to-DMA flow control.
 */
int host1x_channel_enable_dma_flowctrl(struct host1x_channel *channel)
{
	struct host1x *host = dev_get_drvdata(channel->dev->parent);
	struct dma_slave_config dma_sconfig;
	int ret = -ENODEV;

	if (host->dma_chan) {
		dma_sconfig.src_addr       = INDDATA_FIFO_ADDR(channel->id);
		dma_sconfig.src_maxburst   = 1;
		dma_sconfig.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		dma_sconfig.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		dma_sconfig.direction      = DMA_DEV_TO_MEM;
		dma_sconfig.device_fc      = true;

		ret = dmaengine_slave_config(host->dma_chan, &dma_sconfig);
		if (ret) {
			dev_err(channel->dev,
				"Failed to change DMA config %d\n", ret);
			return ret;
		}

		ret = host1x_hw_channel_dma_flowctrl(host, channel, true);
		if (ret) {
			dev_err(channel->dev,
				"Failed to enable DMA flow control %d\n", ret);
			return ret;
		}
	}

	return ret;
}
EXPORT_SYMBOL(host1x_channel_enable_dma_flowctrl);

/**
 * host1x_channel_disable_dma_flowctrl() - Disable DRQ and terminate DMA TX's
 * @channel: Host1x channel that requested DMA to read data
 *
 * Disables channel-to-DMA flow control and terminates all DMA transfers.
 */
void host1x_channel_disable_dma_flowctrl(struct host1x_channel *channel)
{
	struct host1x *host = dev_get_drvdata(channel->dev->parent);

	if (host->dma_chan) {
		host1x_hw_channel_dma_flowctrl(host, channel, false);
		dmaengine_terminate_sync(host->dma_chan);
	}
}
EXPORT_SYMBOL(host1x_channel_disable_dma_flowctrl);
