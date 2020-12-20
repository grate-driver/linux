/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2010 Google, Inc.
 * Author: Erik Gilling <konkers@android.com>
 *
 * Copyright (C) 2011-2017 NVIDIA Corporation
 *
 * Copyright (C) 2019 GRATE-driver project
 */

#if HOST1X_HW < 6
#define HOST1X_CFPEEK_CTRL_ENA_F(v)       HOST1X_SYNC_CFPEEK_CTRL_ENA_F(v)
#define HOST1X_CFPEEK_CTRL_CHANNR_F(v)    HOST1X_SYNC_CFPEEK_CTRL_CHANNR_F(v)
#define HOST1X_CFPEEK_CTRL_ADDR_F(v)      HOST1X_SYNC_CFPEEK_CTRL_ADDR_F(v)
#define HOST1X_CFPEEK_PTRS_CF_RD_PTR_V(v) HOST1X_SYNC_CFPEEK_PTRS_CF_RD_PTR_V(v)
#define HOST1X_CFPEEK_PTRS_CF_WR_PTR_V(v) HOST1X_SYNC_CFPEEK_PTRS_CF_WR_PTR_V(v)
#define HOST1X_CF_SETUP_BASE_V(v)         HOST1X_SYNC_CF_SETUP_BASE_V(v)
#define HOST1X_CF_SETUP_LIMIT_V(v)        HOST1X_SYNC_CF_SETUP_LIMIT_V(v)
#else
#define HOST1X_CFPEEK_CTRL_ENA_F(v)       HOST1X_HV_CMDFIFO_PEEK_CTRL_ENABLE(v)
#define HOST1X_CFPEEK_CTRL_CHANNR_F(v)    HOST1X_HV_CMDFIFO_PEEK_CTRL_CHANNEL(v)
#define HOST1X_CFPEEK_CTRL_ADDR_F(v)      HOST1X_HV_CMDFIFO_PEEK_CTRL_ADDR(v)
#define HOST1X_CFPEEK_PTRS_CF_RD_PTR_V(v) HOST1X_HV_CMDFIFO_PEEK_PTRS_RD_PTR_V(v)
#define HOST1X_CFPEEK_PTRS_CF_WR_PTR_V(v) HOST1X_HV_CMDFIFO_PEEK_PTRS_WR_PTR_V(v)
#define HOST1X_CF_SETUP_BASE_V(v)         HOST1X_HV_CMDFIFO_SETUP_BASE_V(v)
#define HOST1X_CF_SETUP_LIMIT_V(v)        HOST1X_HV_CMDFIFO_SETUP_LIMIT_V(v)
#endif

static int host1x_soc_init_channels(struct host1x *host)
{
	unsigned int i;

	idr_init(&host->channels);
	spin_lock_init(&host->channels_lock);

	/* reset each channel, putting hardware into predictable state */
	for (i = 0; i < HOST1X_CHANNELS_NUM; i++) {
		host1x_hw_channel_stop(host, i);
		host1x_hw_channel_teardown(host, i);
	}

	return 0;
}

static void host1x_soc_deinit_channels(struct host1x *host)
{
	unsigned int i;

	/* shouldn't happen, all channels must be released at this point */
	WARN_ON(!idr_is_empty(&host->channels));

	/* all channels must be stopped now, but let's be extra paranoid */
	for (i = 0; i < HOST1X_CHANNELS_NUM; i++) {
		host1x_hw_channel_stop(host, i);
		host1x_hw_channel_teardown(host, i);
	}

	idr_destroy(&host->channels);
}

static struct host1x_channel *
host1x_soc_channel_request(struct host1x *host, struct device *dev,
			   unsigned int num_pushbuf_words)
{
	struct host1x_channel *chan;
	int ret;

	chan = kzalloc(sizeof(*chan), GFP_KERNEL);
	if (!chan)
		return ERR_PTR(-ENOMEM);

	ret = host1x_soc_pushbuf_init(host, &chan->pb, num_pushbuf_words);
	if (ret)
		goto err_free_chan;

	idr_preload(GFP_KERNEL);
	spin_lock(&host->channels_lock);

	ret = idr_alloc(&host->channels, chan, 0, HOST1X_CHANNELS_NUM,
			GFP_ATOMIC);

	spin_unlock(&host->channels_lock);
	idr_preload_end();

	if (ret < 0)
		goto err_free_pushbuf_bo;

	kref_init(&chan->refcount);
	chan->host = host;
	chan->dev = dev;
	chan->id = ret;

	host1x_hw_channel_init(chan);

	return chan;

err_free_pushbuf_bo:
	host1x_soc_pushbuf_release(host, &chan->pb);
err_free_chan:
	kfree(chan);

	return ERR_PTR(ret);
}

static void host1x_soc_channel_reset(struct host1x_channel *chan)
{
	struct host1x *host = chan->host;

	spin_lock(&host->channels_lock);

	host1x_hw_channel_stop(host, chan->id);
	host1x_hw_channel_teardown(host, chan->id);
	host1x_hw_channel_start(host, chan->id);

	spin_unlock(&host->channels_lock);
}

static void host1x_soc_channel_reinit(struct host1x_channel *chan)
{
	struct host1x *host = chan->host;

	spin_lock(&host->channels_lock);
	host1x_hw_channel_init(chan);
	spin_unlock(&host->channels_lock);
}

static void host1x_soc_channel_stop(struct host1x_channel *chan)
{
	struct host1x *host = chan->host;

	spin_lock(&host->channels_lock);
	host1x_hw_channel_stop(host, chan->id);
	host1x_hw_channel_teardown(host, chan->id);
	spin_unlock(&host->channels_lock);
}

static void host1x_soc_release_channel(struct kref *kref)
{
	struct host1x_channel *chan = container_of(kref, struct host1x_channel,
						   refcount);
	struct host1x *host = chan->host;

	host1x_hw_channel_stop(host, chan->id);
	host1x_hw_channel_teardown(host, chan->id);

	spin_lock(&host->channels_lock);
	idr_remove(&host->channels, chan->id);
	spin_unlock(&host->channels_lock);

	host1x_soc_pushbuf_release(host, &chan->pb);
	kfree(chan);
}

static inline void
host1x_soc_channel_pre_submit(struct host1x_channel *chan,
			      struct host1x_job *job)
{
	struct host1x_syncpt *syncpt = job->syncpt;
	struct host1x *host = chan->host;

	/* set up job's sync point hardware state */
	host1x_hw_syncpt_set_value(host, syncpt->id, 0);
	host1x_hw_syncpt_set_threshold(host, syncpt->id, job->num_incrs + 1);
	host1x_hw_syncpt_set_interrupt(host, syncpt->id, true);

	/*
	 * Both channel's push buffer and job's commands buffer are
	 * write-combined.
	 */
	wmb();
}

static void
host1x_soc_job_fence_callback(struct dma_fence *f, struct dma_fence_cb *cb)
{
	struct host1x_job *job = container_of(cb, struct host1x_job, cb);
	struct host1x_channel *chan = job->chan;

	host1x_soc_pushbuf_pop_job(&chan->pb, job);
}

static struct dma_fence *
host1x_soc_channel_submit(struct host1x_channel *chan,
			  struct host1x_job *job,
			  struct dma_fence *fence)
{
	struct host1x_syncpt *syncpt = job->syncpt;
	struct host1x *host = chan->host;

	/* re-use fence or allocate a new one */
	if (!fence) {
		/*
		 * One more sync point increment will be added by
		 * pushbuf_push_job(), it is necessary because we need to
		 * ensure that CDMA completes job's BO execution before
		 * that BO is released.
		 */
		fence = host1x_fence_create(chan, syncpt,
					    job->num_incrs + 1, job->context);
	}

	if (fence) {
		dma_fence_add_callback(fence, &job->cb,
				       host1x_soc_job_fence_callback);

		spin_lock(&host->channels_lock);

		/* assign channel to this job */
		job->chan = chan;

		host1x_soc_pushbuf_push_job(&chan->pb, job);
		host1x_soc_channel_pre_submit(chan, job);
		host1x_hw_channel_submit(chan, job);

		spin_unlock(&host->channels_lock);
	}

	return fence;
}

static void
host1x_soc_channel_cleanup_job(struct host1x_channel *chan,
			       struct host1x_job *job,
			       struct dma_fence *fence)
{
	dma_fence_remove_callback(fence, &job->cb);
	host1x_soc_pushbuf_pop_job(&chan->pb, job);
}

static dma_addr_t
host1x_soc_channel_dmaget(struct host1x_channel *chan)
{
	return host1x_hw_channel_dmaget(chan->host, chan->id);
}

static void
host1x_soc_dump_channel_fifo_by_id(struct host1x_dbg_output *o,
				   struct host1x *host,
				   unsigned int id)
{
	u32 value, rd_ptr, wr_ptr, start, end;
	u32 payload = INVALID_PAYLOAD;
	unsigned int data_count = 0;

	value = host1x_hw_channel_fifostat(host, id);

	if (HOST1X_CHANNEL_FIFOSTAT_CFEMPTY_V(value)) {
		host1x_debug_output(o, "FIFOSTAT %08x (empty)\n", value);
		return;
	}

	host1x_debug_output(o, "FIFOSTAT %08x\n", value);
	host1x_debug_output(o, "FIFO:\n");

	spin_lock(&host->channels_lock);

	/* peek pointer values are invalid during SLCG, so disable it */
	host1x_hw_channel_icg_en_override(host, 0x1);
	host1x_hw_channel_set_cfpeek_ctrl(host, 0x0);
	host1x_hw_channel_set_cfpeek_ctrl(host,
				HOST1X_CFPEEK_CTRL_ENA_F(1) |
				HOST1X_CFPEEK_CTRL_CHANNR_F(id));

	value = host1x_hw_channel_cfpeek_ptrs(host);
	rd_ptr = HOST1X_CFPEEK_PTRS_CF_RD_PTR_V(value);
	wr_ptr = HOST1X_CFPEEK_PTRS_CF_WR_PTR_V(value);

	value = host1x_hw_channel_cf_setup(host, id);
	start = HOST1X_CF_SETUP_BASE_V(value);
	end = HOST1X_CF_SETUP_LIMIT_V(value);

	spin_unlock(&host->channels_lock);

	do {
		spin_lock(&host->channels_lock);
		host1x_hw_channel_icg_en_override(host, 0x1);
		host1x_hw_channel_set_cfpeek_ctrl(host, 0x0);
		host1x_hw_channel_set_cfpeek_ctrl(host,
					HOST1X_CFPEEK_CTRL_ENA_F(1) |
					HOST1X_CFPEEK_CTRL_CHANNR_F(id) |
					HOST1X_CFPEEK_CTRL_ADDR_F(rd_ptr));
		value = host1x_hw_channel_cfpeek_read(host);
		spin_unlock(&host->channels_lock);

		if (!data_count) {
			host1x_debug_output(o, "%08x: ", value);
			data_count = show_channel_command(o, value, &payload);
		} else {
			host1x_debug_cont(o, "%08x%s", value,
					  data_count > 1 ? ", " : "])\n");
			data_count--;
		}

		if (rd_ptr == end)
			rd_ptr = start;
		else
			rd_ptr++;
	} while (rd_ptr != wr_ptr);

	if (data_count)
		host1x_debug_cont(o, ", ...])\n");
	host1x_debug_output(o, "\n");

	spin_lock(&host->channels_lock);
	host1x_hw_channel_set_cfpeek_ctrl(host, 0x0);
	host1x_hw_channel_icg_en_override(host, 0x0);
	spin_unlock(&host->channels_lock);
}

static void
host1x_soc_dump_channel_by_id(struct host1x_dbg_output *o,
			      struct host1x *host,
			      unsigned int id)
{
	u32 dmactl = host1x_hw_channel_dmactrl(host, id);
	u32 dmaget = host1x_hw_channel_dmaget(host, id);
	u32 dmaput = host1x_hw_channel_dmaput(host, id);
#if HOST1X_HW < 6
	u32 cbstat = host1x_hw_channel_cbstat(host, id);
	u32 word = host1x_hw_channel_cbread(host, id);
	u32 class = HOST1X_SYNC_CBSTAT_CBCLASS_V(cbstat);
	u32 offset = HOST1X_SYNC_CBSTAT_CBOFFSET_V(cbstat);
#else
	u32 word = host1x_hw_channel_cmdfifo_rdata(host, id);
	u32 class = host1x_hw_channel_cmdp_class(host, id);
	u32 offset = host1x_hw_channel_cmdp_offset(host, id);
#endif
	struct host1x_channel *chan;
	char user_name[256];

	spin_lock(&host->channels_lock);

	chan = idr_find(&host->channels, id);
	if (chan)
		snprintf(user_name, ARRAY_SIZE(user_name),
			 "%s", dev_name(chan->dev));

	spin_unlock(&host->channels_lock);

	host1x_debug_output(o, "channel %u hardware state: dmaget %08x, dmaput %08x, active class %02x, offset %04x, val %08x, dmactrl %08x, %s\n",
			    id, dmaget, dmaput, class, offset, word, dmactl,
			    chan ? user_name : "unused");

	host1x_soc_dump_channel_fifo_by_id(o, host, id);
}

static void
host1x_soc_dump_channel(struct host1x_dbg_output *o,
			struct host1x_channel *chan)
{
	host1x_soc_dump_channel_by_id(o, chan->host, chan->id);
}

static void
host1x_soc_dump_channels(struct host1x_dbg_output *o, struct host1x *host)
{
	unsigned int i;

	for (i = 0; i < HOST1X_CHANNELS_NUM; i++) {
		host1x_soc_dump_channel_by_id(o, host, i);
		host1x_soc_dump_channel_fifo_by_id(o, host, i);
	}
}
