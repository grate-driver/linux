// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DMA Driver for Loongson 1 SoC
 *
 * Copyright (C) 2015-2021 Zhang, Keguang <keguang.zhang@gmail.com>
 */

#include <linux/clk.h>
#include <linux/dmapool.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <dma.h>

#include "dmaengine.h"
#include "virt-dma.h"

/* Loongson 1 DMA Register Definitions */
#define LS1X_DMA_CTRL		0x0

/* DMA Control Register Bits */
#define LS1X_DMA_STOP		BIT(4)
#define LS1X_DMA_START		BIT(3)

#define LS1X_DMA_ADDR_MASK	GENMASK(31, 6)

/* DMA Command Register Bits */
#define LS1X_DMA_RAM2DEV		BIT(12)
#define LS1X_DMA_TRANS_OVER		BIT(3)
#define LS1X_DMA_SINGLE_TRANS_OVER	BIT(2)
#define LS1X_DMA_INT			BIT(1)
#define LS1X_DMA_INT_MASK		BIT(0)

struct ls1x_dma_lli {
	u32 next;		/* next descriptor address */
	u32 saddr;		/* memory DMA address */
	u32 daddr;		/* device DMA address */
	u32 length;
	u32 stride;
	u32 cycles;
	u32 cmd;
} __aligned(64);

struct ls1x_dma_hwdesc {
	struct ls1x_dma_lli *lli;
	dma_addr_t phys;
};

struct ls1x_dma_desc {
	struct virt_dma_desc vdesc;
	struct ls1x_dma_chan *chan;

	enum dma_transfer_direction dir;
	enum dma_transaction_type type;

	unsigned int nr_descs;	/* number of descriptors */
	unsigned int nr_done;	/* number of completed descriptors */
	struct ls1x_dma_hwdesc hwdesc[];	/* DMA coherent descriptors */
};

struct ls1x_dma_chan {
	struct virt_dma_chan vchan;
	struct dma_pool *desc_pool;
	struct dma_slave_config cfg;

	unsigned int id;
	void __iomem *reg_base;
	unsigned int irq;

	struct ls1x_dma_desc *desc;
};

struct ls1x_dma {
	struct dma_device ddev;
	struct clk *clk;
	void __iomem *reg_base;

	unsigned int nr_chans;
	struct ls1x_dma_chan chan[];
};

#define to_ls1x_dma_chan(dchan)		\
	container_of(dchan, struct ls1x_dma_chan, vchan.chan)

#define to_ls1x_dma_desc(vdesc)		\
	container_of(vdesc, struct ls1x_dma_desc, vdesc)

/* macros for registers read/write */
#define chan_readl(chan, off)		\
	readl((chan)->reg_base + (off))

#define chan_writel(chan, off, val)	\
	writel((val), (chan)->reg_base + (off))

static bool ls1x_dma_filter(struct dma_chan *chan, void *param);

static inline struct device *chan2dev(struct dma_chan *chan)
{
	return &chan->dev->device;
}

static void ls1x_dma_free_chan_resources(struct dma_chan *dchan)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);

	vchan_free_chan_resources(&chan->vchan);
	dma_pool_destroy(chan->desc_pool);
	chan->desc_pool = NULL;
}

static int ls1x_dma_alloc_chan_resources(struct dma_chan *dchan)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);

	chan->desc_pool = dma_pool_create(dma_chan_name(dchan),
					  dchan->device->dev,
					  sizeof(struct ls1x_dma_lli),
					  __alignof__(struct ls1x_dma_lli), 0);
	if (!chan->desc_pool)
		return -ENOMEM;

	return 0;
}

static void ls1x_dma_free_desc(struct virt_dma_desc *vdesc)
{
	struct ls1x_dma_desc *desc = to_ls1x_dma_desc(vdesc);

	if (desc->nr_descs) {
		unsigned int i = desc->nr_descs;
		struct ls1x_dma_hwdesc *hwdesc;

		do {
			hwdesc = &desc->hwdesc[--i];
			dma_pool_free(desc->chan->desc_pool, hwdesc->lli,
				      hwdesc->phys);
		} while (i);
	}

	kfree(desc);
}

static struct ls1x_dma_desc *ls1x_dma_alloc_desc(struct ls1x_dma_chan *chan, int sg_len)
{
	struct ls1x_dma_desc *desc;

	desc = kzalloc(struct_size(desc, hwdesc, sg_len), GFP_NOWAIT);

	return desc;
}

static struct dma_async_tx_descriptor *
ls1x_dma_prep_slave_sg(struct dma_chan *dchan, struct scatterlist *sgl,
		       unsigned int sg_len,
		       enum dma_transfer_direction direction,
		       unsigned long flags, void *context)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	struct dma_slave_config *cfg = &chan->cfg;
	struct ls1x_dma_desc *desc;
	struct scatterlist *sg;
	unsigned int dev_addr, bus_width, cmd, i;

	if (!is_slave_direction(direction)) {
		dev_err(chan2dev(dchan), "invalid DMA direction!\n");
		return NULL;
	}

	dev_dbg(chan2dev(dchan), "sg_len=%d, dir=%s, flags=0x%lx\n", sg_len,
		direction == DMA_MEM_TO_DEV ? "to device" : "from device",
		flags);

	switch (direction) {
	case DMA_MEM_TO_DEV:
		dev_addr = cfg->dst_addr;
		bus_width = cfg->dst_addr_width;
		cmd = LS1X_DMA_RAM2DEV | LS1X_DMA_INT;
		break;
	case DMA_DEV_TO_MEM:
		dev_addr = cfg->src_addr;
		bus_width = cfg->src_addr_width;
		cmd = LS1X_DMA_INT;
		break;
	default:
		dev_err(chan2dev(dchan),
			"unsupported DMA transfer direction! %d\n", direction);
		return NULL;
	}

	/* allocate DMA descriptor */
	desc = ls1x_dma_alloc_desc(chan, sg_len);
	if (!desc)
		return NULL;

	for_each_sg(sgl, sg, sg_len, i) {
		dma_addr_t buf_addr = sg_dma_address(sg);
		size_t buf_len = sg_dma_len(sg);
		struct ls1x_dma_hwdesc *hwdesc = &desc->hwdesc[i];
		struct ls1x_dma_lli *lli;

		if (!is_dma_copy_aligned(dchan->device, buf_addr, 0, buf_len)) {
			dev_err(chan2dev(dchan), "%s: buffer is not aligned!\n",
				__func__);
			goto err;
		}

		/* allocate HW DMA descriptors */
		lli = dma_pool_alloc(chan->desc_pool, GFP_NOWAIT,
				     &hwdesc->phys);
		if (!lli) {
			dev_err(chan2dev(dchan),
				"%s: failed to alloc HW DMA descriptor!\n",
				__func__);
			goto err;
		}
		hwdesc->lli = lli;

		/* config HW DMA descriptors */
		lli->saddr = buf_addr;
		lli->daddr = dev_addr;
		lli->length = buf_len / bus_width;
		lli->stride = 0;
		lli->cycles = 1;
		lli->cmd = cmd;
		lli->next = 0;

		if (i)
			desc->hwdesc[i - 1].lli->next = hwdesc->phys;

		dev_dbg(chan2dev(dchan),
			"hwdesc=%px, saddr=%08x, daddr=%08x, length=%u\n",
			hwdesc, buf_addr, dev_addr, buf_len);
	}

	/* config DMA descriptor */
	desc->chan = chan;
	desc->dir = direction;
	desc->type = DMA_SLAVE;
	desc->nr_descs = sg_len;
	desc->nr_done = 0;

	return vchan_tx_prep(&chan->vchan, &desc->vdesc, flags);
err:
	desc->nr_descs = i;
	ls1x_dma_free_desc(&desc->vdesc);
	return NULL;
}

static int ls1x_dma_slave_config(struct dma_chan *dchan,
				 struct dma_slave_config *config)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);

	chan->cfg = *config;

	return 0;
}

static int ls1x_dma_terminate_all(struct dma_chan *dchan)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&chan->vchan.lock, flags);

	chan_writel(chan, LS1X_DMA_CTRL,
		    chan_readl(chan, LS1X_DMA_CTRL) | LS1X_DMA_STOP);
	chan->desc = NULL;
	vchan_get_all_descriptors(&chan->vchan, &head);

	spin_unlock_irqrestore(&chan->vchan.lock, flags);

	vchan_dma_desc_free_list(&chan->vchan, &head);

	return 0;
}

static void ls1x_dma_trigger(struct ls1x_dma_chan *chan)
{
	struct dma_chan *dchan = &chan->vchan.chan;
	struct ls1x_dma_desc *desc;
	struct virt_dma_desc *vdesc;
	unsigned int val;

	vdesc = vchan_next_desc(&chan->vchan);
	if (!vdesc) {
		chan->desc = NULL;
		return;
	}
	chan->desc = desc = to_ls1x_dma_desc(vdesc);

	dev_dbg(chan2dev(dchan), "cookie=%d, %u descs, starting hwdesc=%px\n",
		dchan->cookie, desc->nr_descs, &desc->hwdesc[0]);

	val = desc->hwdesc[0].phys & LS1X_DMA_ADDR_MASK;
	val |= chan->id;
	val |= LS1X_DMA_START;
	chan_writel(chan, LS1X_DMA_CTRL, val);
}

static void ls1x_dma_issue_pending(struct dma_chan *dchan)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	unsigned long flags;

	spin_lock_irqsave(&chan->vchan.lock, flags);

	if (vchan_issue_pending(&chan->vchan) && !chan->desc)
		ls1x_dma_trigger(chan);

	spin_unlock_irqrestore(&chan->vchan.lock, flags);
}

static irqreturn_t ls1x_dma_irq_handler(int irq, void *data)
{
	struct ls1x_dma_chan *chan = data;
	struct dma_chan *dchan = &chan->vchan.chan;

	dev_dbg(chan2dev(dchan), "DMA IRQ %d on channel %d\n", irq, chan->id);
	if (!chan->desc) {
		dev_warn(chan2dev(dchan),
			 "DMA IRQ with no active descriptor on channel %d\n", chan->id);
		return IRQ_NONE;
	}

	spin_lock(&chan->vchan.lock);

	if (chan->desc->type == DMA_CYCLIC) {
		vchan_cyclic_callback(&chan->desc->vdesc);
	} else {
		list_del(&chan->desc->vdesc.node);
		vchan_cookie_complete(&chan->desc->vdesc);
		chan->desc = NULL;
	}

	ls1x_dma_trigger(chan);

	spin_unlock(&chan->vchan.lock);
	return IRQ_HANDLED;
}

static int ls1x_dma_chan_probe(struct platform_device *pdev,
			       struct ls1x_dma *dma, int chan_id)
{
	struct device *dev = &pdev->dev;
	struct ls1x_dma_chan *chan = &dma->chan[chan_id];
	char *irq_name;
	int ret;

	chan->irq = platform_get_irq(pdev, chan_id);
	if (chan->irq < 0) {
		dev_err(dev, "failed to get IRQ %d!\n", chan->irq);
		return ret;
	}

	irq_name = devm_kasprintf(dev, GFP_KERNEL, "%s:ch%u",
				  dev_name(dev), chan_id);
	if (!irq_name)
		return -ENOMEM;

	ret = devm_request_irq(dev, chan->irq, ls1x_dma_irq_handler,
			       IRQF_SHARED, irq_name, chan);
	if (ret) {
		dev_err(dev, "failed to request IRQ %u!\n", chan->irq);
		return ret;
	}

	chan->id = chan_id;
	chan->reg_base = dma->reg_base;
	chan->vchan.desc_free = ls1x_dma_free_desc;
	vchan_init(&chan->vchan, &dma->ddev);
	dev_info(dev, "channel %d (irq %d) initialized\n", chan->id, chan->irq);

	return 0;
}

static void ls1x_dma_chan_remove(struct ls1x_dma *dma, int chan_id)
{
	struct device *dev = dma->ddev.dev;
	struct ls1x_dma_chan *chan = &dma->chan[chan_id];

	devm_free_irq(dev, chan->irq, chan);
	list_del(&chan->vchan.chan.device_node);
	tasklet_kill(&chan->vchan.task);
}

static int ls1x_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct plat_ls1x_dma *pdata;
	struct dma_device *ddev;
	struct ls1x_dma *dma;
	int nr_chans, ret, i;

	pdata = dev_get_platdata(dev);
	if (!pdata) {
		dev_err(dev, "platform data missing!\n");
		return -EINVAL;
	}

	nr_chans = platform_irq_count(pdev);
	if (nr_chans <= 0)
		return nr_chans;

	dma = devm_kzalloc(dev, struct_size(dma, chan, nr_chans), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;

	/* initialize DMA device */
	dma->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dma->reg_base))
		return PTR_ERR(dma->reg_base);

	ddev = &dma->ddev;
	ddev->dev = dev;
	ddev->copy_align = DMAENGINE_ALIGN_16_BYTES;
	ddev->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	ddev->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	ddev->directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	ddev->residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	ddev->device_alloc_chan_resources = ls1x_dma_alloc_chan_resources;
	ddev->device_free_chan_resources = ls1x_dma_free_chan_resources;
	ddev->device_prep_slave_sg = ls1x_dma_prep_slave_sg;
	ddev->device_config = ls1x_dma_slave_config;
	ddev->device_terminate_all = ls1x_dma_terminate_all;
	ddev->device_tx_status = dma_cookie_status;
	ddev->device_issue_pending = ls1x_dma_issue_pending;
	ddev->filter.map = pdata->slave_map;
	ddev->filter.mapcnt = pdata->slavecnt;
	ddev->filter.fn = ls1x_dma_filter;

	dma_cap_set(DMA_SLAVE, ddev->cap_mask);
	INIT_LIST_HEAD(&ddev->channels);

	/* initialize DMA channels */
	for (i = 0; i < nr_chans; i++) {
		ret = ls1x_dma_chan_probe(pdev, dma, i);
		if (ret)
			return ret;
	}
	dma->nr_chans = nr_chans;

	dma->clk = devm_clk_get(dev, pdev->name);
	if (IS_ERR(dma->clk)) {
		dev_err(dev, "failed to get %s clock!\n", pdev->name);
		return PTR_ERR(dma->clk);
	}
	clk_prepare_enable(dma->clk);

	ret = dma_async_device_register(ddev);
	if (ret) {
		dev_err(dev, "failed to register DMA device! %d\n", ret);
		clk_disable_unprepare(dma->clk);
		return ret;
	}

	platform_set_drvdata(pdev, dma);
	dev_info(dev, "Loongson1 DMA driver registered\n");

	return 0;
}

static int ls1x_dma_remove(struct platform_device *pdev)
{
	struct ls1x_dma *dma = platform_get_drvdata(pdev);
	int i;

	dma_async_device_unregister(&dma->ddev);
	clk_disable_unprepare(dma->clk);
	for (i = 0; i < dma->nr_chans; i++)
		ls1x_dma_chan_remove(dma, i);

	return 0;
}

static struct platform_driver ls1x_dma_driver = {
	.probe	= ls1x_dma_probe,
	.remove	= ls1x_dma_remove,
	.driver	= {
		.name	= "ls1x-dma",
	},
};

module_platform_driver(ls1x_dma_driver);

static bool ls1x_dma_filter(struct dma_chan *dchan, void *param)
{
	struct ls1x_dma_chan *chan = to_ls1x_dma_chan(dchan);
	unsigned int chan_id = (unsigned int)param;

	if (dchan->device->dev->driver != &ls1x_dma_driver.driver)
		return false;

	return chan_id == chan->id;
}

MODULE_AUTHOR("Kelvin Cheung <keguang.zhang@gmail.com>");
MODULE_DESCRIPTION("Loongson1 DMA driver");
MODULE_LICENSE("GPL");
