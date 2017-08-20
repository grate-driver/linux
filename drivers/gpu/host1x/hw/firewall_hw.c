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

#include "../dev.h"
#include "../firewall.h"

#define HOST1X_INCR_SYNCPT_OFFSET 0x0
#define HOST1X_WAIT_SYNCPT_OFFSET 0x8

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

static bool check_wait(struct host1x_waitchk *wait, struct host1x_bo *cmdbuf,
		       unsigned int offset)
{
	offset *= sizeof(u32);

	if (wait->bo != cmdbuf) {
		FW_ERR("Doesn't belong to cmdbuf\n");
		return false;
	}

	if (wait->offset != offset) {
		FW_ERR("Invalid offset 0x%X\n", wait->offset);
		return false;
	}

	return true;
}

static int check_register(struct host1x_firewall *fw, unsigned long offset,
			  bool immediate)
{
	/* assume that all modules have INCR_SYNCPT at the same offset */
	if (HOST1X_HW < 6 && offset == HOST1X_INCR_SYNCPT_OFFSET) {
		u32 word = fw->cmdbuf_base[fw->offset];
		unsigned int cond = (word >> 8) & 0xff;
		unsigned int syncpt_id = word & 0xff;
		unsigned int i;

		if (!fw->syncpt_incrs) {
			FW_ERR("Invalid number of syncpoints\n");
			return -EINVAL;
		}

		/*
		 * Syncpoint increment must be the last command of the
		 * stream in order to ensure that all outstanding HW
		 * operations complete before jobs fence would signal.
		 */
		if (fw->syncpt_incrs == 1) {
			if (!fw->last || fw->words != (immediate ? 0 : 1)) {
				FW_ERR("Syncpoint increment must be the last "
				       "command in a stream\n");
				return -EINVAL;
			}

			/* condition must be OP_DONE */
			if (cond != 1) {
				FW_ERR("Invalid last syncpoint condition code "
				       "%u, should be 1 (OP_DONE)\n", cond);
				return -EINVAL;
			}
		}

		/* Check whether syncpoint belongs to jobs client */
		for (i = 0; i < fw->job->client->num_syncpts; i++) {
			struct host1x_syncpt *sp = fw->job->client->syncpts[i];

			if (host1x_syncpt_id(sp) == syncpt_id)
				goto valid_syncpt;
		}

		FW_ERR("Syncpoint ID %u doesn't belong to the client\n",
		       syncpt_id);

		return -EINVAL;

valid_syncpt:
		fw->syncpt_incrs--;
	}

	/* skip unnecessary validations on Tegra30+ */
	if (fw->iommu)
		return 0;

	if (fw->job->is_addr_reg &&
	    fw->job->is_addr_reg(fw->dev, offset)) {
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

	if (offset == HOST1X_WAIT_SYNCPT_OFFSET) {
		if (fw->class != HOST1X_CLASS_HOST1X) {
			FW_ERR("Jobs class must be 'host1x' for a waitcheck\n");
			return -EINVAL;
		}

		if (!fw->num_waitchks) {
			FW_ERR("Invalid number of a waitchecks\n");
			return -EINVAL;
		}

		if (!check_wait(fw->waitchk, fw->cmdbuf, fw->offset))
			return -EINVAL;

		fw->num_waitchks--;
		fw->waitchk++;
	}

	return 0;
}

static int check_class(struct host1x_firewall *fw, u32 class)
{
#if HOST1X_HW < 4
	if (fw->class != class) {
		FW_ERR("Invalid class ID 0x%X, should be 0x%X\n",
		       class, fw->class);
		return -EINVAL;
	}
#endif
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

static int firewall_validate_gather(struct host1x_firewall *fw,
				    struct host1x_job_gather *g,
				    bool last_gather)
{
	u32 *cmdbuf_base = (u32 *)fw->job->gather_copy_mapped +
		(g->offset / sizeof(u32));
	int err = 0;

	fw->cmdbuf_base = cmdbuf_base;
	fw->last = last_gather;
	fw->words = g->words;
	fw->cmdbuf = g->bo;
	fw->offset = 0;

	if (!fw->syncpt_incrs) {
		FW_ERR("Invalid number of syncpoints\n");
		return -EINVAL;
	}

	while (fw->words && !err) {
		u32 word = cmdbuf_base[fw->offset];
		u32 opcode = (word & 0xf0000000) >> 28;

		fw->mask = 0;
		fw->reg = 0;
		fw->count = 0;
		fw->words--;
		fw->offset++;

		switch (opcode) {
		case HOST1X_OPCODE_SETCLASS:
			fw->mask = word & 0x3f;
			fw->reg = word >> 16 & 0xfff;
			err = check_class(fw, word >> 6 & 0x3ff);
			if (!err)
				err = check_mask(fw);
			break;

		case HOST1X_OPCODE_INCR:
			fw->reg = word >> 16 & 0xfff;
			fw->count = word & 0xffff;
			err = check_incr(fw);
			break;

		case HOST1X_OPCODE_NONINCR:
			fw->reg = word >> 16 & 0xfff;
			fw->count = word & 0xffff;
			err = check_nonincr(fw);
			break;

		case HOST1X_OPCODE_MASK:
			fw->mask = word & 0xffff;
			fw->reg = word >> 16 & 0xfff;
			err = check_mask(fw);
			break;

		case HOST1X_OPCODE_IMM:
			fw->reg = word >> 16 & 0x1fff;
			err = check_register(fw, fw->reg, true);
			if (err)
				fw->offset--;
			break;

		case HOST1X_OPCODE_RESTART:
		case HOST1X_OPCODE_GATHER:
		case HOST1X_OPCODE_EXTEND:
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

	return err;
}

static bool firewall_needs_validation(bool iommu)
{
	return HOST1X_HW < 6 || !iommu;
}

static const struct host1x_firewall_ops host1x_firewall_ops = {
	.validate_gather = firewall_validate_gather,
	.needs_validation = firewall_needs_validation,
};
