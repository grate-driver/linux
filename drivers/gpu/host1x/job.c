/*
 * Tegra host1x Job
 *
 * Copyright (c) 2010-2015, NVIDIA Corporation.
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

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/host1x.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <trace/events/host1x.h>

#include "channel.h"
#include "dev.h"
#include "job.h"
#include "syncpt.h"

#define HOST1X_INCR_SYNCPT_OFFSET 0x0
#define HOST1X_WAIT_SYNCPT_OFFSET 0x8

#define FW_ERR(fmt, args...) \
	pr_err("HOST1X firewall: %s: " fmt, __func__, ##args)

struct host1x_job *host1x_job_alloc(struct host1x_channel *ch,
				    u32 num_cmdbufs, u32 num_relocs)
{
	struct host1x_job *job = NULL;
	unsigned int num_unpins = num_relocs;
	u64 total;
	void *mem;

	if (!IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL))
		num_unpins += num_cmdbufs;

	/* Check that we're not going to overflow */
	total = sizeof(struct host1x_job) +
		(u64)num_relocs * sizeof(struct host1x_reloc) +
		(u64)num_unpins * sizeof(struct host1x_job_unpin_data) +
		(u64)num_cmdbufs * sizeof(struct host1x_job_gather) +
		(u64)num_unpins * sizeof(dma_addr_t) +
		(u64)num_unpins * sizeof(u32 *);
	if (total > ULONG_MAX)
		return NULL;

	mem = job = kzalloc(total, GFP_KERNEL);
	if (!job)
		return NULL;

	kref_init(&job->ref);
	job->channel = ch;

	/* Redistribute memory to the structs  */
	mem += sizeof(struct host1x_job);
	job->relocs = num_relocs ? mem : NULL;
	mem += num_relocs * sizeof(struct host1x_reloc);
	job->unpins = num_unpins ? mem : NULL;
	mem += num_unpins * sizeof(struct host1x_job_unpin_data);
	job->gathers = num_cmdbufs ? mem : NULL;
	mem += num_cmdbufs * sizeof(struct host1x_job_gather);
	job->addr_phys = num_unpins ? mem : NULL;

	job->reloc_addr_phys = job->addr_phys;
	job->gather_addr_phys = &job->addr_phys[num_relocs];

	return job;
}
EXPORT_SYMBOL(host1x_job_alloc);

struct host1x_job *host1x_job_get(struct host1x_job *job)
{
	kref_get(&job->ref);
	return job;
}
EXPORT_SYMBOL(host1x_job_get);

static void job_free(struct kref *ref)
{
	struct host1x_job *job = container_of(ref, struct host1x_job, ref);

	kfree(job);
}

void host1x_job_put(struct host1x_job *job)
{
	kref_put(&job->ref, job_free);
}
EXPORT_SYMBOL(host1x_job_put);

void host1x_job_add_gather(struct host1x_job *job, struct host1x_bo *bo,
			   unsigned int words, unsigned int offset)
{
	struct host1x_job_gather *gather = &job->gathers[job->num_gathers];

	gather->words = words;
	gather->bo = bo;
	gather->offset = offset;

	job->num_gathers++;
}
EXPORT_SYMBOL(host1x_job_add_gather);

static unsigned int pin_job(struct host1x *host, struct host1x_job *job)
{
	struct host1x_bo *bo = NULL;
	unsigned int i;
	int err;

	job->num_unpins = 0;

	for (i = 0; i < job->num_relocs; i++) {
		struct host1x_reloc *reloc = &job->relocs[i];
		struct sg_table *sgt;
		dma_addr_t phys_addr;

		bo = host1x_bo_get(reloc->target.bo);
		if (!bo) {
			err = -EINVAL;
			goto unpin;
		}

		err = host1x_bo_pin(bo, &phys_addr, &sgt);
		if (err)
			goto unpin;

		job->addr_phys[job->num_unpins] = phys_addr;
		job->unpins[job->num_unpins].bo = bo;
		job->unpins[job->num_unpins].sgt = sgt;
		job->num_unpins++;
	}

	/*
	 * We will copy gathers BO content later, so there is no need to
	 * hold and pin them.
	 */
	if (IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL))
		return 0;

	for (i = 0; i < job->num_gathers; i++) {
		struct host1x_job_gather *g = &job->gathers[i];
		size_t gather_size = 0;
		struct scatterlist *sg;
		struct sg_table *sgt;
		dma_addr_t phys_addr;
		unsigned long shift;
		struct iova *alloc;
		unsigned int j;

		bo = host1x_bo_get(g->bo);
		if (!bo) {
			err = -EINVAL;
			goto unpin;
		}

		err = host1x_bo_pin(bo, &phys_addr, &sgt);
		if (err)
			goto unpin;

		if (host->domain) {
			for_each_sg(sgt->sgl, sg, sgt->nents, j)
				gather_size += sg->length;
			gather_size = iova_align(&host->iova, gather_size);

			shift = iova_shift(&host->iova);
			alloc = alloc_iova(&host->iova, gather_size >> shift,
					   host->iova_end >> shift, true);
			if (!alloc) {
				err = -ENOMEM;
				goto unpin;
			}

			err = iommu_map_sg(host->domain,
					iova_dma_addr(&host->iova, alloc),
					sgt->sgl, sgt->nents, IOMMU_READ);
			if (err == 0) {
				__free_iova(&host->iova, alloc);
				err = -EINVAL;
				goto unpin;
			}

			job->addr_phys[job->num_unpins] =
				iova_dma_addr(&host->iova, alloc);
			job->unpins[job->num_unpins].size = gather_size;
		} else {
			job->addr_phys[job->num_unpins] = phys_addr;
		}

		job->gather_addr_phys[i] = job->addr_phys[job->num_unpins];

		job->unpins[job->num_unpins].bo = bo;
		job->unpins[job->num_unpins].sgt = sgt;
		job->num_unpins++;
	}

	return 0;

unpin:
	if (bo)
		host1x_bo_put(bo);
	host1x_job_unpin(job);
	return err;
}

static int do_relocs(struct host1x_job *job, struct host1x_job_gather *g)
{
	u32 last_page = ~0;
	void *cmdbuf_page_addr = NULL;
	struct host1x_bo *cmdbuf = g->bo;
	unsigned int i;

	/* pin & patch the relocs for one gather */
	for (i = 0; i < job->num_relocs; i++) {
		struct host1x_reloc *reloc = &job->relocs[i];
		u32 reloc_addr = (job->reloc_addr_phys[i] +
				  reloc->target.offset) >> reloc->shift;
		u32 *target;

		/* skip all other gathers */
		if (cmdbuf != reloc->cmdbuf.bo)
			continue;

		if (IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL)) {
			target = (u32 *)job->gather_copy_mapped +
					reloc->cmdbuf.offset / sizeof(u32) +
						g->offset / sizeof(u32);
			goto patch_reloc;
		}

		if (last_page != reloc->cmdbuf.offset >> PAGE_SHIFT) {
			if (cmdbuf_page_addr)
				host1x_bo_kunmap(cmdbuf, last_page,
						 cmdbuf_page_addr);

			cmdbuf_page_addr = host1x_bo_kmap(cmdbuf,
					reloc->cmdbuf.offset >> PAGE_SHIFT);
			last_page = reloc->cmdbuf.offset >> PAGE_SHIFT;

			if (unlikely(!cmdbuf_page_addr)) {
				pr_err("Could not map cmdbuf for relocation\n");
				return -ENOMEM;
			}
		}

		target = cmdbuf_page_addr + (reloc->cmdbuf.offset & ~PAGE_MASK);
patch_reloc:
		*target = reloc_addr;
	}

	if (cmdbuf_page_addr)
		host1x_bo_kunmap(cmdbuf, last_page, cmdbuf_page_addr);

	return 0;
}

static bool check_reloc(struct host1x_reloc *reloc, struct host1x_bo *cmdbuf,
			unsigned int offset)
{
	offset *= sizeof(u32);

	if (reloc->cmdbuf.bo != cmdbuf) {
		FW_ERR("Doesn't belong to cmdbuf\n");
		return false;
	}

	if (reloc->cmdbuf.offset != offset) {
		FW_ERR("Invalid command buffer offset 0x%lX\n",
		       reloc->cmdbuf.offset);
		return false;
	}

	/* relocation shift value validation isn't implemented yet */
	if (reloc->shift) {
		FW_ERR("Shifting is forbidden\n");
		return false;
	}

	return true;
}

struct host1x_firewall {
	struct host1x_job *job;
	struct device *dev;

	unsigned int num_relocs;
	struct host1x_reloc *reloc;

	struct host1x_bo *cmdbuf;
	unsigned int offset;

	unsigned int syncpt_incrs;

	u32 *cmdbuf_base;
	u32 words;
	u32 class;
	u32 reg;
	u32 mask;
	u32 count;
};

static int check_register(struct host1x_firewall *fw, unsigned long offset,
			  bool immediate)
{
	if (fw->job->is_addr_reg &&
	    fw->job->is_addr_reg(fw->dev, fw->class, offset)) {
		if (immediate) {
			FW_ERR("Writing an immediate value to address "
			       "register\n");
			return -EINVAL;
		}

		if (!fw->num_relocs) {
			FW_ERR("Invalid number of relocations\n");
			return -EINVAL;
		}

		if (!check_reloc(fw->reloc, fw->cmdbuf, fw->offset))
			return -EINVAL;

		fw->num_relocs--;
		fw->reloc++;
	}

	/* assume that all modules have INCR_SYNCPT at the same offset */
	if (offset == HOST1X_INCR_SYNCPT_OFFSET) {
		u32 word = fw->cmdbuf_base[fw->offset];
		unsigned int syncpt_id = word & 0xff;

		if (!fw->syncpt_incrs) {
			FW_ERR("Invalid number of syncpoints\n");
			return -EINVAL;
		}

		if (syncpt_id != fw->job->syncpt_id) {
			FW_ERR("Invalid syncpoint ID %u, should be %u\n",
			       syncpt_id, fw->job->syncpt_id);
			return -EINVAL;
		}

		fw->syncpt_incrs--;
	}

	if (offset == HOST1X_WAIT_SYNCPT_OFFSET) {
		FW_ERR("WAIT_SYNCPT opcode forbidden\n");
		return -EINVAL;
	}

	return 0;
}

static int check_class(struct host1x_firewall *fw, u32 class)
{
	if (!fw->job->is_valid_class) {
		if (fw->class != class) {
			FW_ERR("Invalid class ID 0x%X, should be 0x%X\n",
			       fw->class, class);
			return -EINVAL;
		}
	} else {
		if (!fw->job->is_valid_class(fw->class)) {
			FW_ERR("Invalid class ID 0x%X\n", fw->class);
			return -EINVAL;
		}
	}

	return 0;
}

static int check_mask(struct host1x_firewall *fw)
{
	u32 mask = fw->mask;
	u32 reg = fw->reg;
	int ret;

	while (mask) {
		if (fw->words == 0) {
			FW_ERR("Invalid write mask\n");
			return -EINVAL;
		}

		if (mask & 1) {
			ret = check_register(fw, reg, false);
			if (ret < 0)
				return ret;

			fw->words--;
			fw->offset++;
		}
		mask >>= 1;
		reg++;
	}

	return 0;
}

static int check_incr(struct host1x_firewall *fw)
{
	u32 count = fw->count;
	u32 reg = fw->reg;
	int ret;

	while (count) {
		if (fw->words == 0) {
			FW_ERR("Invalid words count\n");
			return -EINVAL;
		}

		ret = check_register(fw, reg, false);
		if (ret < 0)
			return ret;

		reg++;
		fw->words--;
		fw->offset++;
		count--;
	}

	return 0;
}

static int check_nonincr(struct host1x_firewall *fw)
{
	u32 count = fw->count;
	int ret;

	while (count) {
		if (fw->words == 0) {
			FW_ERR("Invalid words count\n");
			return -EINVAL;
		}

		ret = check_register(fw, fw->reg, false);
		if (ret < 0)
			return ret;

		fw->words--;
		fw->offset++;
		count--;
	}

	return 0;
}

static int validate(struct host1x_firewall *fw, struct host1x_job_gather *g)
{
	u32 *cmdbuf_base = (u32 *)fw->job->gather_copy_mapped +
		(g->offset / sizeof(u32));
	u32 job_class = fw->class;
	int err = 0;

	fw->cmdbuf_base = cmdbuf_base;
	fw->words = g->words;
	fw->cmdbuf = g->bo;
	fw->offset = 0;

	while (fw->words && !err) {
		u32 word = cmdbuf_base[fw->offset];
		u32 opcode = (word & 0xf0000000) >> 28;

		fw->mask = 0;
		fw->reg = 0;
		fw->count = 0;
		fw->words--;
		fw->offset++;

		switch (opcode) {
		case 0:
			fw->class = word >> 6 & 0x3ff;
			fw->mask = word & 0x3f;
			fw->reg = word >> 16 & 0xfff;
			err = check_class(fw, job_class);
			if (!err)
				err = check_mask(fw);
			if (err)
				goto out;
			break;
		case 1:
			fw->reg = word >> 16 & 0xfff;
			fw->count = word & 0xffff;
			err = check_incr(fw);
			if (err)
				goto out;
			break;

		case 2:
			fw->reg = word >> 16 & 0xfff;
			fw->count = word & 0xffff;
			err = check_nonincr(fw);
			if (err)
				goto out;
			break;

		case 3:
			fw->mask = word & 0xffff;
			fw->reg = word >> 16 & 0xfff;
			err = check_mask(fw);
			if (err)
				goto out;
			break;
		case 4:
			fw->reg = word >> 16 & 0x1fff;
			err = check_register(fw, fw->reg, true);
			if (err) {
				fw->offset--;
				goto out;
			}
			break;
		case 5:
		case 6:
		case 14:
			FW_ERR("Forbidden command\n");
			err = -EINVAL;
			fw->offset--;
			break;
		default:
			FW_ERR("Invalid command\n");
			err = -EINVAL;
			fw->offset--;
			break;
		}
	}

out:
	return err;
}

static inline int copy_gathers(struct host1x_job *job, struct device *dev)
{
	struct host1x_firewall fw;
	size_t size = 0;
	size_t offset = 0;
	unsigned int i;

	fw.job = job;
	fw.dev = dev;
	fw.reloc = job->relocs;
	fw.num_relocs = job->num_relocs;
	fw.syncpt_incrs = job->syncpt_incrs;
	fw.class = job->class;

	for (i = 0; i < job->num_gathers; i++) {
		struct host1x_job_gather *g = &job->gathers[i];

		size += g->words * sizeof(u32);
	}

	/*
	 * Try a non-blocking allocation from a higher priority pools first,
	 * as awaiting for the allocation here is a major performance hit.
	 */
	job->gather_copy_mapped = dma_alloc_wc(dev, size, &job->gather_copy,
					       GFP_NOWAIT);

	/* the higher priority allocation failed, try the generic-blocking */
	if (!job->gather_copy_mapped)
		job->gather_copy_mapped = dma_alloc_wc(dev, size,
						       &job->gather_copy,
						       GFP_KERNEL);
	if (!job->gather_copy_mapped)
		return -ENOMEM;

	job->gather_copy_size = size;

	for (i = 0; i < job->num_gathers; i++) {
		struct host1x_job_gather *g = &job->gathers[i];
		void *gather;

		/* Copy the gather */
		gather = host1x_bo_mmap(g->bo);
		memcpy(job->gather_copy_mapped + offset, gather + g->offset,
		       g->words * sizeof(u32));
		host1x_bo_munmap(g->bo, gather);

		/* Store the location in the buffer */
		g->base = job->gather_copy;
		g->offset = offset;

		/* Validate the job */
		if (validate(&fw, g)) {
			dev_err(dev, "Command stream validation failed at word "
				"%u of gather #%d, checked %zu words totally\n",
				fw.offset, i, offset / sizeof(u32) + fw.offset);
			return -EINVAL;
		}

		offset += g->words * sizeof(u32);
	}

	/* No relocs and syncpts should remain at this point */
	if (!fw.num_relocs && !fw.syncpt_incrs)
		return 0;

	if (fw.num_relocs)
		FW_ERR("Job has invalid number of relocations, %u left\n",
		       fw.num_relocs);

	if (fw.syncpt_incrs)
		FW_ERR("Job has invalid number of syncpoint increments, "
		       "%u left\n", fw.syncpt_incrs);

	return -EINVAL;
}

int host1x_job_pin(struct host1x_job *job, struct device *dev)
{
	int err;
	unsigned int i, j;
	struct host1x *host = dev_get_drvdata(dev->parent);

	/* pin memory */
	err = pin_job(host, job);
	if (err)
		goto out;

	if (IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL)) {
		err = copy_gathers(job, dev);
		if (err)
			goto out;
	}

	/* patch gathers */
	for (i = 0; i < job->num_gathers; i++) {
		struct host1x_job_gather *g = &job->gathers[i];

		/* process each gather mem only once */
		if (g->handled)
			continue;

		/* copy_gathers() sets gathers base if firewall is enabled */
		if (!IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL))
			g->base = job->gather_addr_phys[i];

		for (j = i + 1; j < job->num_gathers; j++) {
			if (job->gathers[j].bo == g->bo) {
				job->gathers[j].handled = true;
				job->gathers[j].base = g->base;
			}
		}

		err = do_relocs(job, g);
		if (err)
			break;
	}

out:
	if (err)
		host1x_job_unpin(job);
	wmb();

	return err;
}
EXPORT_SYMBOL(host1x_job_pin);

void host1x_job_unpin(struct host1x_job *job)
{
	struct host1x *host = dev_get_drvdata(job->channel->dev->parent);
	unsigned int i;

	for (i = 0; i < job->num_unpins; i++) {
		struct host1x_job_unpin_data *unpin = &job->unpins[i];

		if (!IS_ENABLED(CONFIG_TEGRA_HOST1X_FIREWALL) &&
		    unpin->size && host->domain) {
			iommu_unmap(host->domain, job->addr_phys[i],
				    unpin->size);
			free_iova(&host->iova,
				iova_pfn(&host->iova, job->addr_phys[i]));
		}

		host1x_bo_unpin(unpin->bo, unpin->sgt);
		host1x_bo_put(unpin->bo);
	}

	if (job->gather_copy_size)
		dma_free_wc(job->channel->dev, job->gather_copy_size,
			    job->gather_copy_mapped, job->gather_copy);

	job->num_unpins = 0;
	job->gather_copy_size = 0;
}
EXPORT_SYMBOL(host1x_job_unpin);

/*
 * Debug routine used to dump job entries
 */
void host1x_job_dump(struct device *dev, struct host1x_job *job)
{
	dev_dbg(dev, "    SYNCPT_ID   %d\n", job->syncpt_id);
	dev_dbg(dev, "    SYNCPT_VAL  %d\n", job->syncpt_end);
	dev_dbg(dev, "    FIRST_GET   0x%x\n", job->first_get);
	dev_dbg(dev, "    TIMEOUT     %d\n", job->timeout);
	dev_dbg(dev, "    NUM_SLOTS   %d\n", job->num_slots);
	dev_dbg(dev, "    NUM_HANDLES %d\n", job->num_unpins);
}
