/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2010 Google, Inc.
 * Author: Erik Gilling <konkers@android.com>
 *
 * Copyright (C) 2011-2017 NVIDIA Corporation
 *
 * Copyright (C) 2019 GRATE-driver project
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>

#define CH_DMASTART(chid)	(host->base_regs + HOST1X_CHANNEL_DMASTART(chid))
#define CH_DMAPUT(chid)		(host->base_regs + HOST1X_CHANNEL_DMAPUT(chid))
#define CH_DMAGET(chid)		(host->base_regs + HOST1X_CHANNEL_DMAGET(chid))
#define CH_DMAEND(chid)		(host->base_regs + HOST1X_CHANNEL_DMAEND(chid))
#define CH_DMACTRL(chid)	(host->base_regs + HOST1X_CHANNEL_DMACTRL(chid))
#define CH_FIFOSTAT(chid)	(host->base_regs + HOST1X_CHANNEL_FIFOSTAT(chid))

#if HOST1X_HW < 6

#define CH_CMDPROC_STOP		(host->base_regs + HOST1X_SYNC_OFFSET + HOST1X_SYNC_CMDPROC_STOP)
#define CH_TEARDOWN		(host->base_regs + HOST1X_SYNC_OFFSET + HOST1X_SYNC_CH_TEARDOWN)
#define CH_CBREAD(chid)		(host->base_regs + HOST1X_SYNC_OFFSET + HOST1X_SYNC_CBREAD(chid))
#define CH_CBSTAT(chid)		(host->base_regs + HOST1X_SYNC_OFFSET + HOST1X_SYNC_CBSTAT(chid))

#if HOST1X_HW >= 4
#define CH_CHANNELCTRL(chid)	(host->base_regs + HOST1X_CHANNEL_CHANNELCTRL(chid))
#endif

#define CH_CFPEEK_CTRL		(host->base_regs + HOST1X_SYNC_OFFSET + HOST1X_SYNC_CFPEEK_CTRL)
#define CH_CFPEEK_PTRS		(host->base_regs + HOST1X_SYNC_OFFSET + HOST1X_SYNC_CFPEEK_PTRS)
#define CH_CF_SETUP(chid)	(host->base_regs + HOST1X_SYNC_OFFSET + HOST1X_SYNC_CF_SETUP(chid))
#define CH_CFPEEK_READ		(host->base_regs + HOST1X_SYNC_OFFSET + HOST1X_SYNC_CFPEEK_READ)

#else

#define CH_DMASTART_HI(chid)	(host->base_regs + HOST1X_CHANNEL_DMASTART_HI(chid))
#define CH_DMAEND_HI(chid)	(host->base_regs + HOST1X_CHANNEL_DMAEND_HI(chid))
#define CH_CMDPROC_STOP(chid)	(host->base_regs + HOST1X_CHANNEL_CMDPROC_STOP(chid))
#define CH_TEARDOWN(chid)	(host->base_regs + HOST1X_CHANNEL_TEARDOWN(chid))
#define CH_CMDFIFO_RDATA(chid)	(host->base_regs + HOST1X_CHANNEL_CMDFIFO_RDATA(chid))
#define CH_CMDP_OFFSET(chid)	(host->base_regs + HOST1X_CHANNEL_CMDP_OFFSET(chid))
#define CH_CMDP_CLASS(chid)	(host->base_regs + HOST1X_CHANNEL_CMDP_CLASS(chid))
#define CH_CMDP_CLASS(chid)	(host->base_regs + HOST1X_CHANNEL_CMDP_CLASS(chid))
#define CH_CFPEEK_PTRS		(host->base_regs + HOST1X_HV_CMDFIFO_PEEK_PTRS)
#define CH_CF_SETUP(chid)	(host->base_regs + HOST1X_HV_CMDFIFO_SETUP(chid))
#define CH_CFPEEK_READ		(host->base_regs + HOST1X_HV_CMDFIFO_PEEK_READ)
#define CH_ICG_EN_OVERRIDE	(host->base_regs + HOST1X_HV_ICG_EN_OVERRIDE)

#define CH_CFPEEK_CTRL				(host->hv_regs + HOST1X_HV_CMDFIFO_PEEK_CTRL)
#define HV_CH_KERNEL_FILTER_GBUFFER(idx)	(host->hv_regs + HOST1X_HV_CH_KERNEL_FILTER_GBUFFER(idx))

#endif

static inline
void host1x_hw_channel_stop(struct host1x *host, unsigned int id)
{
#if HOST1X_HW < 6
	u32 value;

	/* stop issuing commands from the command FIFO */
	value = readl_relaxed(CH_CMDPROC_STOP);
	writel_relaxed(value | BIT(id), CH_CMDPROC_STOP);
#else
	writel_relaxed(1, CH_CMDPROC_STOP(id));
#endif

	/* stop DMA from fetching on this channel */
	writel_relaxed(HOST1X_CHANNEL_DMACTRL_DMASTOP, CH_DMACTRL(id));
}

static inline
void host1x_hw_channel_start(struct host1x *host, unsigned int id)
{
#if HOST1X_HW < 6
	u32 value;

	value = readl_relaxed(CH_CMDPROC_STOP);
	writel_relaxed(value & ~BIT(id), CH_CMDPROC_STOP);
#else
	writel_relaxed(0, CH_CMDPROC_STOP(id));
#endif
	/* set DMAGET = DMAPUT */
	writel_relaxed(HOST1X_CHANNEL_DMACTRL_DMASTOP |
		       HOST1X_CHANNEL_DMACTRL_DMAGETRST |
		       HOST1X_CHANNEL_DMACTRL_DMAINITGET, CH_DMACTRL(id));

	/* prevent delaying before all writes committed */
	wmb();

	/* cyndis: setting DMAGET takes 4 cycles */
	udelay(10);

	/* stop holding DMA in a paused state, now */
	writel_relaxed(0x0, CH_DMACTRL(id));
}

static inline void
host1x_hw_channel_teardown(struct host1x *host, unsigned int id)
{
	/*
	 * Reset channel's command FIFO and release any locks it has in
	 * the arbiter.
	 */
#if HOST1X_HW < 6
	writel_relaxed(BIT(id), CH_TEARDOWN);
#else
	writel_relaxed(1, CH_TEARDOWN(id));
#endif
}

static inline u32
host1x_hw_channel_dmaget(struct host1x *host, unsigned int id)
{
	return readl_relaxed(CH_DMAGET(id));
}

static inline u32
host1x_hw_channel_dmaput(struct host1x *host, unsigned int id)
{
	return readl_relaxed(CH_DMAPUT(id));
}

static inline u32
host1x_hw_channel_dmactrl(struct host1x *host, unsigned int id)
{
	return readl_relaxed(CH_DMACTRL(id));
}

static inline u32
host1x_hw_channel_fifostat(struct host1x *host, unsigned int id)
{
	return readl_relaxed(CH_FIFOSTAT(id));
}

#if HOST1X_HW < 6
static inline u32
host1x_hw_channel_cbread(struct host1x *host, unsigned int id)
{
	return readl_relaxed(CH_CBREAD(id));
}

static inline u32
host1x_hw_channel_cbstat(struct host1x *host, unsigned int id)
{
	return readl_relaxed(CH_CBSTAT(id));
}

static inline void
host1x_hw_channel_icg_en_override(struct host1x *host, u32 value)
{
}
#else
static inline u32
host1x_hw_channel_cmdfifo_rdata(struct host1x *host, unsigned int id)
{
	return readl_relaxed(CH_CMDFIFO_RDATA(id));
}

static inline u32
host1x_hw_channel_cmdp_offset(struct host1x *host, unsigned int id)
{
	return readl_relaxed(CH_CMDP_OFFSET(id));
}

static inline u32
host1x_hw_channel_cmdp_class(struct host1x *host, unsigned int id)
{
	return readl_relaxed(CH_CMDP_CLASS(id));
}

static inline void
host1x_hw_channel_icg_en_override(struct host1x *host, u32 value)
{
	return writel_relaxed(value, CH_ICG_EN_OVERRIDE);
}
#endif

static inline void
host1x_hw_channel_set_cfpeek_ctrl(struct host1x *host, u32 value)
{
	return writel_relaxed(value, CH_CFPEEK_CTRL);
}

static inline u32
host1x_hw_channel_cfpeek_ptrs(struct host1x *host)
{
	return readl_relaxed(CH_CFPEEK_PTRS);
}

static inline u32
host1x_hw_channel_cf_setup(struct host1x *host, unsigned int id)
{
	return readl_relaxed(CH_CF_SETUP(id));
}

static inline u32
host1x_hw_channel_cfpeek_read(struct host1x *host)
{
	return readl_relaxed(CH_CFPEEK_READ);
}

static inline void
host1x_hw_channel_init(struct host1x_channel *chan)
{
	struct host1x_pushbuf *pb = &chan->pb;
	struct host1x *host = chan->host;
	unsigned int id = chan->id;
	u32 dma_end;
#if HOST1X_HW >= 6
	u32 value;
#endif
	/* reset hardware state */
	host1x_hw_channel_stop(chan->host, chan->id);
	host1x_hw_channel_teardown(chan->host, chan->id);

	/*
	 * Keep DMA on hold while updating addresses since any update
	 * triggers the memory fetching process.
	 */
	writel_relaxed(HOST1X_CHANNEL_DMACTRL_DMASTOP, CH_DMACTRL(id));

	/*
	 * Set DMASTART to 0x0, DMAGET and DMAPUT will be treated as absolute
	 * addresses in this case.
	 *
	 * Note that DMASTART/END must be programmed before GET/PUT,
	 * otherwise it's undefined behavior and CDMA may start fetching
	 * from a wrong address when DMASTOP is deasserted.
	 */
	writel_relaxed(0x00000000, CH_DMASTART(id));

	/*
	 * Tegra20 has Host1x v01 and Tegra20 has GART for IOMMU which ends
	 * at 0x60000000.
	 */
	if (of_machine_is_compatible("nvidia,tegra20"))
		dma_end = 0x60000000;
	else
		dma_end = 0xffffffff;

	/* do not limit DMA addressing */
	writel_relaxed(dma_end, CH_DMAEND(id));

#if HOST1X_HW >= 6
	/* set upper halves of the addresses */
	writel_relaxed(0x00000000, CH_DMASTART_HI(id));
	writel_relaxed(0xffffffff, CH_DMAEND_HI(id));

	/* enable setclass command filter for gather buffers */
	spin_lock(&host->channels_lock);

	value = readl_relaxed(HV_CH_KERNEL_FILTER_GBUFFER(id / 32));

	writel_relaxed(value | BIT(id % 32),
		       HV_CH_KERNEL_FILTER_GBUFFER(id / 32));

	spin_unlock(&host->channels_lock);
#elif HOST1X_HW >= 4
	writel_relaxed(HOST1X_CHANNEL_CHANNELCTRL_KERNEL_FILTER_GBUFFER(1),
		       CH_CHANNELCTRL(id));
#endif
	/* set DMAPUT to push buffer's put */
	writel_relaxed(host1x_soc_pushbuf_dmaput_addr(pb), CH_DMAPUT(id));

	host1x_hw_channel_start(host, id);
}

static inline void
host1x_hw_channel_submit(struct host1x_channel *chan,
			 struct host1x_job *job)
{
	struct host1x_pushbuf *pb = &chan->pb;
	struct host1x *host = chan->host;
	unsigned int id = chan->id;

	/* trigger DMA execution (DMAGET != DMAPUT) */
	writel_relaxed(host1x_soc_pushbuf_dmaput_addr(pb), CH_DMAPUT(id));
}
