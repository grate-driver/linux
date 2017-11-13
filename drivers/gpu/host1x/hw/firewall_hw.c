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

/**
 * firewall_enable_gather_filter() - Enable gather filter
 * @sp: syncpoint
 * @ch: channel
 *
 * On chips with the gather filter HW firewall feature (Tegra124+), enable
 * basic HW firewall that would stop CDMA execution on trying to execute
 * forbidden commands: SETCLASS, SETSTRMID and EXTEND.
 *
 * On older chips, do nothing.
 */
static void firewall_enable_gather_filter(struct host1x *host,
					  struct host1x_channel *ch)
{
#if HOST1X_HW >= 6
	u32 val;

	if (!host->hv_regs)
		return;

	val = host1x_hypervisor_readl(
		host, HOST1X_HV_CH_KERNEL_FILTER_GBUFFER(ch->id / 32));
	val |= BIT(ch->id % 32);
	host1x_hypervisor_writel(
		host, val, HOST1X_HV_CH_KERNEL_FILTER_GBUFFER(ch->id / 32));
#elif HOST1X_HW >= 4
	host1x_ch_writel(ch,
			 HOST1X_CHANNEL_CHANNELCTRL_KERNEL_FILTER_GBUFFER(1),
			 HOST1X_CHANNEL_CHANNELCTRL);
#endif
}

/**
 * firewall_syncpt_assign_to_channel() - Assign syncpoint to channel
 * @sp: syncpoint
 * @ch: channel
 *
 * On chips with the syncpoint protection feature (Tegra186+), assign @sp to
 * @ch, preventing other channels from incrementing the syncpoints. If @ch is
 * NULL, unassigns the syncpoint.
 *
 * On older chips, do nothing.
 */
static void firewall_syncpt_assign_to_channel(struct host1x_syncpt *sp,
					      struct host1x_channel *ch)
{
#if HOST1X_HW >= 6
	host1x_sync_writel(sp->host,
			   HOST1X_SYNC_SYNCPT_CH_APP_CH(ch ? ch->id : 0xff),
			   HOST1X_SYNC_SYNCPT_CH_APP(sp->id));
#endif
}

/**
 * firewall_enable_syncpt_protection() - Enable syncpoint protection
 * @host: host1x instance
 *
 * On chips with the syncpoint protection feature (Tegra186+), enable this
 * feature. On older chips, do nothing.
 */
static void firewall_enable_syncpt_protection(struct host1x *host)
{
#if HOST1X_HW >= 6
	if (!host->hv_regs)
		return;

	host1x_hypervisor_writel(host,
				 HOST1X_HV_SYNCPT_PROT_EN_CH_EN,
				 HOST1X_HV_SYNCPT_PROT_EN);
#endif
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

static int check_register(struct host1x_firewall *fw, bool immediate,
			  unsigned int writes_num)
{
	if (fw->job->is_addr_reg &&
	    fw->job->is_addr_reg(fw->dev, fw->class, fw->reg)) {
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

		return 0;
	}

	/* assume that all modules have INCR_SYNCPT at the same offset */
	if (fw->reg == HOST1X_UCLASS_INCR_SYNCPT) {
		while (writes_num--) {
			u32 word = fw->cmdbuf_base[fw->offset + writes_num];
			unsigned int syncpt_id = word & 0xff;

			if (!fw->syncpt_incrs) {
				FW_ERR("Invalid number of syncpoints\n");
				return -EINVAL;
			}

			if (syncpt_id != fw->job->syncpt->id) {
				FW_ERR("Invalid syncpoint ID %u, "
				       "should be %u\n",
				       syncpt_id, fw->job->syncpt->id);
				return -EINVAL;
			}

			fw->syncpt_incrs--;

			if (fw->syncpt_incrs < 0) {
				FW_ERR("Invalid number of syncpoints\n");
				return -EINVAL;
			}
		}
	}

	if (fw->reg == HOST1X_UCLASS_WAIT_SYNCPT) {
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

		return 0;
	}

	return 0;
}

static int check_class(struct host1x_firewall *fw)
{
	if (!fw->job->is_valid_class)
		return 0;

	if (!fw->job->is_valid_class(fw->class)) {
		FW_ERR("Invalid class ID 0x%X\n", fw->class);
		return -EINVAL;
	}

	return 0;
}

static int check_mask(struct host1x_firewall *fw)
{
	int err;

	while (fw->mask) {
		if (fw->words == 0) {
			FW_ERR("Invalid write mask\n");
			return -EINVAL;
		}

		if (fw->mask & 1) {
			err = check_register(fw, false, 1);
			if (err)
				return err;

			fw->words--;
			fw->offset++;
		}
		fw->mask >>= 1;
		fw->reg++;
	}

	return 0;
}

static int check_incr(struct host1x_firewall *fw)
{
	int err;

	while (fw->count--) {
		if (fw->words == 0) {
			FW_ERR("Invalid words count\n");
			return -EINVAL;
		}

		err = check_register(fw, false, 1);
		if (err)
			return err;

		fw->reg++;
		fw->words--;
		fw->offset++;
	}

	return 0;
}

static int check_nonincr(struct host1x_firewall *fw)
{
	int err;

	if (fw->words == 0) {
		FW_ERR("Invalid words count\n");
		return -EINVAL;
	}

	if (fw->count == 0)
		return 0;

	err = check_register(fw, false, fw->count);
	if (err)
		return err;

	fw->words  -= fw->count;
	fw->offset += fw->count;

	return 0;
}

static int firewall_validate_gather(struct host1x_firewall *fw,
				    struct host1x_job_gather *g)
{
	u32 *cmdbuf_base = (u32 *)fw->job->gather_copy_mapped +
		(g->offset / sizeof(u32));
	int ret = 0;

	fw->cmdbuf_base = cmdbuf_base;
	fw->words = g->words;
	fw->cmdbuf = g->bo;
	fw->offset = 0;

	while (fw->words && !ret) {
		u32 word = cmdbuf_base[fw->offset];
		u32 opcode = (word & 0xf0000000) >> 28;

		fw->mask = 0;
		fw->reg = 0;
		fw->count = 0;
		fw->words--;
		fw->offset++;

		switch (opcode) {
		case HOST1X_OPCODE_SETCLASS:
			fw->class = word >> 6 & 0x3ff;
			fw->mask = word & 0x3f;
			fw->reg = word >> 16 & 0xfff;
			ret = check_class(fw);
			if (ret == 0)
				ret = check_mask(fw);
			break;

		case HOST1X_OPCODE_INCR:
			fw->reg = word >> 16 & 0xfff;
			fw->count = word & 0xffff;
			ret = check_incr(fw);
			break;

		case HOST1X_OPCODE_NONINCR:
			fw->reg = word >> 16 & 0xfff;
			fw->count = word & 0xffff;
			ret = check_nonincr(fw);
			break;

		case HOST1X_OPCODE_MASK:
			fw->mask = word & 0xffff;
			fw->reg = word >> 16 & 0xfff;
			ret = check_mask(fw);
			break;

		case HOST1X_OPCODE_IMM:
			fw->reg = word >> 16 & 0x1fff;
			ret = check_register(fw, true, 1);
			if (ret)
				fw->offset--;
			break;

		case HOST1X_OPCODE_RESTART:
		case HOST1X_OPCODE_GATHER:
		case HOST1X_OPCODE_EXTEND:
			FW_ERR("Forbidden command\n");
			ret = -EINVAL;
			fw->offset--;
			break;

		default:
			FW_ERR("Invalid command\n");
			ret = -EINVAL;
			fw->offset--;
			break;
		}
	}

	return ret;
}

static const struct host1x_firewall_ops host1x_firewall_ops = {
	.validate_gather = firewall_validate_gather,
	.enable_gather_filter = firewall_enable_gather_filter,
	.syncpt_assign_to_channel = firewall_syncpt_assign_to_channel,
	.enable_syncpt_protection = firewall_enable_syncpt_protection,
};
