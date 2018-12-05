/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/bitmap.h>
#include <linux/interrupt.h>
#include <linux/io.h>

#define SYNCPT(idx)							\
	(host->base_regs + HOST1X_SYNC_OFFSET +				\
		HOST1X_SYNC_SYNCPT(idx))

#define SYNCPT_THRESH_CPU0_INT_STATUS(idx)				\
	(host->base_regs + HOST1X_SYNC_OFFSET +				\
		HOST1X_SYNC_SYNCPT_THRESH_CPU0_INT_STATUS(idx))

#define SYNCPT_THRESH_CPU0_INT_DISABLE(idx)				\
	(host->base_regs + HOST1X_SYNC_OFFSET +				\
		HOST1X_SYNC_SYNCPT_THRESH_INT_DISABLE(idx))

#define SYNCPT_THRESH_CPU0_INT_ENABLE(idx)				\
	(host->base_regs + HOST1X_SYNC_OFFSET +				\
		HOST1X_SYNC_SYNCPT_THRESH_INT_ENABLE_CPU0(idx))

#define SYNCPT_INT_THRESH(idx)						\
	(host->base_regs + HOST1X_SYNC_OFFSET +				\
		HOST1X_SYNC_SYNCPT_INT_THRESH(idx))

#if HOST1X_HW < 6

#define SYNC_IP_BUSY_TIMEOUT						\
	(host->base_regs + HOST1X_SYNC_OFFSET +				\
		HOST1X_SYNC_IP_BUSY_TIMEOUT)

#define SYNC_CTXSW_TIMEOUT_CFG						\
	(host->base_regs + HOST1X_SYNC_OFFSET +				\
		HOST1X_SYNC_CTXSW_TIMEOUT_CFG)

#define SYNC_USEC_CLK							\
	(host->base_regs + HOST1X_SYNC_OFFSET +				\
		HOST1X_SYNC_USEC_CLK)

#else

#define HV_SYNCPT_PROT_EN						\
	(host->hv_regs + HOST1X_HV_SYNCPT_PROT_EN)

#endif

static inline void host1x_syncpt_signal_fence(struct host1x_fence *fence)
{
	/* detach fence from sync point */
	list_del(&fence->list);

	/* signal about expiration */
	dma_fence_signal_locked(&fence->base);

	/* drop refcount, note this may release the fence */
	dma_fence_put(&fence->base);
}

static inline bool host1x_syncpt_expired(u32 value, u32 threshold)
{
	return (s32)(value - threshold) >= 0;
}

static inline bool
host1x_hw_syncpt_handled(struct host1x *host,
			 struct host1x_syncpt *syncpt,
			 unsigned int id)
{
	struct host1x_fence *fence, *tmp;
	bool handled = false;
	u32 syncpt_value;

	/*
	 * If list contains single entry, then there is no
	 * need to check the threshold value because we already
	 * know that threshold is reached for this entry (it is
	 * likely to be the most common case).
	 */
	if (list_is_singular(&syncpt->fences)) {
		fence = list_first_entry(&syncpt->fences, struct host1x_fence,
					 list);

		host1x_syncpt_signal_fence(fence);
		handled = true;
	} else {
		syncpt_value = readl_relaxed(SYNCPT(id));

		list_for_each_entry_safe(fence, tmp, &syncpt->fences, list) {

			if (host1x_syncpt_expired(syncpt_value,
						  fence->syncpt_thresh)) {
				host1x_syncpt_signal_fence(fence);
				handled = true;
			}
		}
	}

	if (!list_empty(&syncpt->fences))
		return handled;

	/* mask interrupt if we are done with this sync point */
	writel_relaxed(BIT(id % 32), SYNCPT_THRESH_CPU0_INT_DISABLE(id / 32));

	/* mark sync point as inactive */
	if (HOST1X_SYNCPTS_NUM > 32)
		clear_bit(id, host->active_syncpts);

	return handled;
}

static inline struct host1x_syncpt *
host1x_lookup_syncpt(struct host1x *host, unsigned int id)
{
	struct host1x_syncpt *syncpt = idr_find(&host->syncpts, id);

	/* shouldn't happen */
	if (unlikely(!syncpt || list_empty(&syncpt->fences))) {
		dev_err_ratelimited(host->dev,
				    "isr: erroneously active sync point %u\n",
				    id);
		writel_relaxed(BIT(id % 32),
			       SYNCPT_THRESH_CPU0_INT_DISABLE(id / 32));

		return NULL;
	}

	return syncpt;
}

static inline unsigned int
host1x_next_active_syncpt_id(struct host1x *host, unsigned int id)
{
	/* optimize code a tad for older HW that has 32 sync points */
	if (HOST1X_SYNCPTS_NUM == 32)
		return 0;

	return find_next_bit(host->active_syncpts, HOST1X_SYNCPTS_NUM, id);
}

static irqreturn_t host1x_hw_syncpt_isr(int irq, void *data)
{
	struct host1x_syncpt *syncpt;
	struct host1x *host = data;
	irqreturn_t status = IRQ_NONE;
	unsigned int base_id = 0;
	unsigned int next_id;
	unsigned int id = 0;
	unsigned long value;
	unsigned int i;

	spin_lock(&host1x_syncpts_lock);

	do {
		next_id = host1x_next_active_syncpt_id(host, id);

		if (next_id == HOST1X_SYNCPTS_NUM) {
			/* done if all active sync points were handled */
			if (status == IRQ_HANDLED)
				break;
			/*
			 * Otherwise some sync point fired erroneously,
			 * we are going to find out that bad sync point
			 * and report it.
			 */
		} else {
			next_id = id;
		}

		/* read interrupt-status of the pending sync points */
		value = readl_relaxed(SYNCPT_THRESH_CPU0_INT_STATUS(id / 32));

		/* handle up to 32 sync points */
		if (HOST1X_SYNCPTS_NUM > 32)
			base_id = round_down(id, 32);

		/* handle each bit that is set in the interrupt-status value */
		for_each_set_bit(i, &value, 32) {
			id = base_id + i;

			syncpt = host1x_lookup_syncpt(host, id);
			if (!syncpt)
				continue;

			/*
			 * Handle sync point and mark interrupt as handled if
			 * one of the fences signalled.
			 */
			if (host1x_hw_syncpt_handled(host, syncpt, id))
				status = IRQ_HANDLED;
		}

		/* clear interrupt-status of the handled sync points */
		writel_relaxed(value, SYNCPT_THRESH_CPU0_INT_STATUS(id / 32));

		/* move on to the next 32 sync points */
		id = base_id + 32;

	} while (HOST1X_SYNCPTS_NUM > 32 && id < HOST1X_SYNCPTS_NUM);

	spin_unlock(&host1x_syncpts_lock);

	return status;
}

static inline void
host1x_hw_syncpt_set_interrupt(struct host1x *host, unsigned int id,
			       bool enable)
{
	if (enable)
		writel_relaxed(BIT(id % 32),
			       SYNCPT_THRESH_CPU0_INT_ENABLE(id / 32));
	else
		writel_relaxed(BIT(id % 32),
			       SYNCPT_THRESH_CPU0_INT_DISABLE(id / 32));
}

static inline void
host1x_hw_syncpt_clr_intr_sts(struct host1x *host, unsigned int id)
{
	writel_relaxed(BIT(id % 32), SYNCPT_THRESH_CPU0_INT_STATUS(id / 32));
}

static inline void
host1x_hw_syncpt_set_value(struct host1x *host, unsigned int id, u32 value)
{
	writel_relaxed(value, SYNCPT(id));
}

static inline void
host1x_hw_syncpt_set_threshold(struct host1x *host, unsigned int id,
			       u32 thresh)
{
	writel_relaxed(thresh, SYNCPT_INT_THRESH(id));
}

static inline u32
host1x_hw_syncpt_value(struct host1x *host, unsigned int id)
{
	return readl_relaxed(SYNCPT(id));
}

static inline u32
host1x_hw_syncpt_thresh(struct host1x *host, unsigned int id)
{
	return readl_relaxed(SYNCPT_INT_THRESH(id));
}

static inline bool
host1x_hw_syncpt_intr_status(struct host1x *host, unsigned int id)
{
	u32 status;

	status = readl_relaxed(SYNCPT_THRESH_CPU0_INT_STATUS(id / 32));

	return !!(status & BIT(id % 32));
}

static inline void
host1x_hw_init_syncpts(struct host1x *host)
{
	unsigned int i;
#if HOST1X_HW < 6
	u32 clk = clk_get_rate(host->clk);
	u32 cpm = DIV_ROUND_UP(clk, 1000000);

	/* disable the ip_busy_timeout, this prevents write drops */
	writel_relaxed(0, SYNC_IP_BUSY_TIMEOUT);

	/*
	 * Increase the auto-ack timout to the maximum value, 2d may hang
	 * otherwise on Tegra20.
	 */
	writel_relaxed(0xff, SYNC_CTXSW_TIMEOUT_CFG);

	/* update host clocks per usec */
	writel_relaxed(cpm, SYNC_USEC_CLK);
#else
	/* enable sync point protection */
	writel_relaxed(HOST1X_HV_SYNCPT_PROT_EN_CH_EN, HV_SYNCPT_PROT_EN);
#endif

	/* make sure that sync points won't fire up after IRQ requesting */
	for (i = 0; i < HOST1X_SYNCPTS_NUM / 32; i++) {
		writel_relaxed(0xffffffff, SYNCPT_THRESH_CPU0_INT_DISABLE(i));
		writel_relaxed(0xffffffff, SYNCPT_THRESH_CPU0_INT_STATUS(i));
	}
}
