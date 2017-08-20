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

int host1x_firewall_check_job(struct host1x *host, struct host1x_job *job,
			      struct device *dev)
{
	struct host1x_syncpt *sp;
	unsigned int i;

	sp = host1x_syncpt_get(host, job->syncpt_id);
	if (!sp) {
		FW_ERR("Jobs syncpoint ID %u is invalid\n", job->syncpt_id);
		goto fail;
	}

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
			       "offset %lu\n", i, reloc->target.offset);
			goto fail;
		}

		if (reloc->target.offset >= host1x_bo_size(reloc->target.bo)) {
			FW_ERR("Relocation #%u has invalid target "
			       "offset %lu, max %zu\n",
			       i, reloc->target.offset,
			       host1x_bo_size(reloc->target.bo));
			goto fail;
		}

		if (reloc->cmdbuf.offset & 3) {
			FW_ERR("Relocation #%u has unaligned cmdbuf "
			       "offset %lu\n", i, reloc->cmdbuf.offset);
			goto fail;
		}

		if (reloc->cmdbuf.offset >= host1x_bo_size(reloc->cmdbuf.bo)) {
			FW_ERR("Relocation #%u has invalid cmdbuf "
			       "offset %lu, max %zu\n",
			       i, reloc->cmdbuf.offset,
			       host1x_bo_size(reloc->cmdbuf.bo));
			goto fail;
		}
	}

	for (i = 0; i < job->num_waitchk; i++) {
		struct host1x_waitchk *wait = &job->waitchk[i];

		sp = host1x_syncpt_get(host, wait->syncpt_id);
		if (!sp) {
			FW_ERR("Waitcheck #%u has invalid syncpoint ID %u\n",
			       i, wait->syncpt_id);
			goto fail;
		}

		if (wait->offset & 3) {
			FW_ERR("Waitcheck #%u has unaligned offset 0x%X\n",
			       i, wait->offset);
			goto fail;
		}

		if (wait->offset >= host1x_bo_size(wait->bo)) {
			FW_ERR("Waitcheck #%u has invalid offset 0x%X, "
			       "max %zu\n", i, wait->offset,
			       host1x_bo_size(wait->bo));
			goto fail;
		}
	}

	return 0;

fail:
	/* print final error message, giving a clue about jobs client */
	dev_err(dev, "Job checking failed\n");
	return -EINVAL;
}

int host1x_firewall_copy_gathers(struct host1x *host, struct host1x_job *job,
				 struct device *dev)
{
	struct host1x_firewall fw;
	size_t offset = 0;
	size_t size = 0;
	unsigned int i;
	bool iommu;

	/* note that in case of Tegra20 we skipped IOMMU initialization */
	iommu = !!host->domain;

	/* skip software firewall on Tegra186 + IOMMU */
	if (!host1x_hw_firewall_needs_validation(host, iommu))
		return 0;

	fw.dev = dev;
	fw.job = job;
	fw.iommu = iommu;
	fw.class = job->class;
	fw.reloc = job->relocarray;
	fw.waitchk = job->waitchk;
	fw.num_relocs = job->num_relocs;
	fw.num_waitchks = job->num_waitchk;
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

	for (i = 0; i < job->num_gathers; i++) {
		struct host1x_job_gather *g = &job->gathers[i];
		bool last_gather = (i == job->num_gathers - 1);
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
		if (host1x_hw_firewall_validate(host, &fw, g, last_gather)) {
			/* convert offset to number of words */
			offset /= sizeof(u32);
			offset += fw.offset + 1;

			FW_ERR("Debug dump:\n");

			host1x_debug_dump_gather(host, g, offset);

			dev_err(dev, "Command stream validation failed at word "
				"%u of gather #%d, checked %zu words totally\n",
				fw.offset, i, offset);

			return -EINVAL;
		}

		offset += g->words * sizeof(u32);
	}

	/* No relocs, waitchks and syncpts should remain at this point */
	if (!fw.num_relocs && !fw.num_waitchks && !fw.syncpt_incrs)
		return 0;

	FW_ERR("Debug dump:\n");

	for (i = 0; i < job->num_gathers; i++)
		host1x_debug_dump_gather(host, &job->gathers[i],
					 CDMA_GATHER_MAX_FETCHES_NB);

	if (fw.num_relocs)
		FW_ERR("Job has invalid number of relocations, %u left\n",
		       fw.num_relocs);

	if (fw.num_waitchks)
		FW_ERR("Job has invalid number of waitchecks, %u left\n",
		       fw.num_waitchks);

	if (fw.syncpt_incrs)
		FW_ERR("Job has invalid number of syncpoint increments, "
		       "%u left\n", fw.syncpt_incrs);

	return -EINVAL;
}
