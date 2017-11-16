/*
 * Copyright (c) 2012-2015, NVIDIA Corporation.
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

#include "dev.h"
#include "debug.h"
#include "firewall.h"

#define CDMA_GATHER_MAX_FETCHES_NB 16383

static void host1x_firewall_dump_gather(struct host1x *host1x,
					struct host1x_job *job,
					struct host1x_job_gather *g,
					unsigned int max_words)
{
	struct output o = {
		.fn = write_to_printk
	};
	unsigned int words;
	u32 *mapped;

	host1x_debug_output(&o, "GATHER at %pad+%#x, %d words, class 0x%X\n",
			    &g->base, g->offset, g->words, g->class);

	if (job->gather_copy_mapped)
		mapped = job->gather_copy_mapped;
	else
		mapped = host1x_bo_mmap(g->bo);

	if (!mapped) {
		dev_err(host1x->dev, "%s: Failed to mmap gather\n", __func__);
		return;
	}

	words = min(g->words, max_words);

	host1x_hw_show_gather(host1x, &o, g->base + g->offset, words, g->base,
			      mapped);

	if (!job->gather_copy_mapped)
		host1x_bo_munmap(g->bo, mapped);
}

int host1x_firewall_check_job(struct host1x *host, struct host1x_job *job,
			      struct device *dev)
{
	unsigned int i;

	host1x_debug_output_lock();

	for (i = 0; i < job->num_gathers; i++) {
		struct host1x_job_gather *g = &job->gathers[i];
		u64 gather_size = g->words * sizeof(u32);

		/*
		 * Gather buffer base address must be 4-bytes aligned,
		 * unaligned offset is malformed and cause commands stream
		 * corruption on the buffers address relocation.
		 */
		if (g->offset & 3) {
			FW_ERR("Gather #%u has unaligned offset %u\n",
			       i, g->offset);
			goto fail;
		}

		/*
		 * The maximum number of CDMA gather fetches is 16383, a higher
		 * value means the words count is malformed.
		 */
		if (g->words > CDMA_GATHER_MAX_FETCHES_NB) {
			FW_ERR("Gather #%u has too many words %u, max %u\n",
			       i, g->words, CDMA_GATHER_MAX_FETCHES_NB);
			goto fail;
		}

		/* Verify that gather is inside of its BO bounds */
		if (g->offset + gather_size > host1x_bo_size(g->bo)) {
			FW_ERR("Gather #%u is malformed: offset %u, "
			       "words %u, BO size %zu\n",
			       i, g->offset, g->words, host1x_bo_size(g->bo));
			goto fail;
		}
	}

	for (i = 0; i < job->num_relocs; i++) {
		struct host1x_reloc *reloc = &job->relocarray[i];

		if (reloc->target.offset & 3) {
			FW_ERR("Relocation #%u has unaligned target "
			       "offset %u\n", i, reloc->target.offset);
			goto fail;
		}

		if (reloc->target.offset >= host1x_bo_size(reloc->target.bo)) {
			FW_ERR("Relocation #%u has invalid target "
			       "offset %u, max %zu\n",
			       i, reloc->target.offset,
			       host1x_bo_size(reloc->target.bo) - sizeof(u32));
			goto fail;
		}

		if (reloc->cmdbuf.offset & 3) {
			FW_ERR("Relocation #%u has unaligned cmdbuf "
			       "offset %u\n", i, reloc->cmdbuf.offset);
			goto fail;
		}

		if (reloc->cmdbuf.index >= job->num_gathers) {
			FW_ERR("Relocation #%u has invalid gather_index %u, "
			       "max %u\n",
			       i, reloc->cmdbuf.index, job->num_gathers - 1);
			goto fail;
		}
	}

	for (i = 0; i < job->num_waitchks; i++) {
		struct host1x_waitchk *waitchk = &job->waitchks[i];
		struct host1x_syncpt *sp;

		sp = host1x_syncpt_get_by_id(host, waitchk->syncpt_id);
		if (!sp) {
			FW_ERR("Waitcheck #%u has invalid syncpoint ID %u\n",
			       i, waitchk->syncpt_id);
			goto fail;
		}

		if (waitchk->relative && !sp->base) {
			FW_ERR("Waitcheck #%u uses syncpoint ID %u which "
			       "doesn't have a base\n",
			       i, waitchk->syncpt_id);
			goto fail;
		}

		if (waitchk->gather_index >= job->num_gathers) {
			FW_ERR("Waitcheck #%u has invalid gather_index %u, "
			       "max %u\n",
			       i, waitchk->gather_index, job->num_gathers - 1);
			goto fail;
		}
	}

	host1x_debug_output_unlock();

	return 0;

fail:
	FW_ERR("Debug dump:\n");

	for (i = 0; i < job->num_gathers; i++) {
		struct host1x_job_gather *g = &job->gathers[i];
		unsigned bo_words = host1x_bo_size(g->bo) / sizeof(u32);

		host1x_firewall_dump_gather(host, job, &job->gathers[i],
					    min(g->words, bo_words));
	}

	/* print final error message, giving a clue about jobs client */
	dev_err(dev, "Job checking failed\n");

	host1x_debug_output_unlock();

	return -EINVAL;
}

int host1x_firewall_copy_gathers(struct host1x *host, struct host1x_job *job,
				 struct device *dev)
{
	struct host1x_firewall fw;
	size_t offset = 0;
	size_t size = 0;
	unsigned int i, k;

	fw.dev = dev;
	fw.job = job;
	fw.class = job->class;
	fw.reloc = job->relocarray;
	fw.num_relocs = job->num_relocs;
	fw.syncpt_incrs = job->syncpt_incrs;

	for (i = 0; i < job->num_gathers; i++)
		size += job->gathers[i].words * sizeof(u32);

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

	host1x_debug_output_lock();

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

		/* Validate jobs gather */
		if (host1x_hw_firewall_validate(host, &fw, g, i)) {
			/* convert offset to number of words */
			offset /= sizeof(u32);
			offset += fw.offset + 1;

			FW_ERR("Debug dump:\n");

			for (k = 0; k <= i; k++)
				host1x_firewall_dump_gather(
						host, job, &job->gathers[k],
						CDMA_GATHER_MAX_FETCHES_NB);

			dev_err(dev, "Command stream validation failed at "
				     "word #%u of gather #%d, checked %zu "
				     "words totally\n",
				fw.offset, i, offset);

			host1x_debug_output_unlock();

			return -EINVAL;
		}

		offset += g->words * sizeof(u32);
	}

	/* No relocs and syncpts should remain at this point */
	if (fw.num_relocs ||fw.syncpt_incrs)
		goto fw_err;

	host1x_debug_output_unlock();

	return 0;

fw_err:
	FW_ERR("Debug dump:\n");

	for (i = 0; i < job->num_gathers; i++)
		host1x_firewall_dump_gather(host, job, &job->gathers[i],
					    CDMA_GATHER_MAX_FETCHES_NB);

	if (fw.num_relocs)
		FW_ERR("Job has invalid number of relocations, %u left\n",
		       fw.num_relocs);

	if (fw.syncpt_incrs)
		FW_ERR("Job has invalid number of syncpoint increments, "
		       "%u left\n", fw.syncpt_incrs);

	host1x_debug_output_unlock();

	return -EINVAL;
}
