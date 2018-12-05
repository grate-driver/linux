/* SPDX-License-Identifier: GPL-2.0 */

#include "job.h"

#define PATCH_ERROR(fmt, args...) \
	DRM_ERROR_RATELIMITED(fmt " (%s)\n", ##args, ps->drm_job->task_name)

struct parser_state {
	struct tegra_drm_job *drm_job;
	struct tegra_drm_client *drm_client;
	u64 pipes_expected;
	u64 pipes;
	const struct tegra_drm *tegra;
	struct tegra_bo *const *bos;
	const unsigned long *addr_regs;
	u32 *words_in;
	u32 word_id;
	const u32 num_words;
	const u16 num_bos;
	const u16 syncpt_id;
	u16 syncpt_incrs;
	u16 count;
	u16 offset;
	u16 mask;
	u16 last_reg;
	u16 num_regs;
	u16 classid;
	u8 opcode;
};

static inline bool
cmdstream_gart_bo(struct parser_state *ps, unsigned int bo_index)
{
	struct tegra_drm_job *drm_job = ps->drm_job;

	if (!IS_ENABLED(CONFIG_TEGRA_IOMMU_GART) || !ps->tegra->has_gart)
		return false;

	return test_bit(bo_index, drm_job->bos_gart_bitmap);
}

static inline int
cmdstream_patch_reloc(struct parser_state *ps,
		      unsigned int offset,
		      const struct tegra_bo **reloc_bo,
		      unsigned int *reloc_offset,
		      bool word_sized_offset,
		      bool want_gather)
{
	struct drm_tegra_cmdstream_reloc reloc_desc;
	const struct tegra_bo *bo;
	size_t max_size;
	u32 *reloc_ptr;
	bool is_gather;

	reloc_ptr = &ps->words_in[ps->word_id + offset];
	reloc_desc.u_data = *reloc_ptr;

	if (reloc_desc.bo_index >= ps->num_bos) {
		PATCH_ERROR("invalid reloc bo index %u, num_bos %u",
			    reloc_desc.bo_index, ps->num_bos);
		return -EINVAL;
	}

	bo = ps->bos[reloc_desc.bo_index];
	offset = reloc_desc.bo_offset;
	max_size = bo->gem.size;
	is_gather = !!(bo->flags & TEGRA_BO_HOST1X_GATHER);

	if (is_gather != want_gather) {
		PATCH_ERROR("invalid reloc bo type");
		return -EINVAL;
	}

	if (word_sized_offset)
		offset *= sizeof(u32);

	if (offset >= max_size) {
		PATCH_ERROR("invalid reloc bo offset %u, gem size %zu",
			    offset, max_size);
		return -EINVAL;
	}

	if (is_gather && offset + ps->count * sizeof(u32) > max_size) {
		PATCH_ERROR("invalid gather size: offset %u, words %u, max size %zu",
			    offset, ps->count, max_size);
		return -EINVAL;
	}

	if (!is_gather && cmdstream_gart_bo(ps, reloc_desc.bo_index))
		*reloc_ptr = bo->gartaddr + offset;
	else
		*reloc_ptr = bo->dmaaddr + offset;

	if (reloc_bo) {
		*reloc_bo = bo;
		*reloc_offset = offset;
	}

	return 0;
}

static inline int cmdstream_patch_relocs(struct parser_state *ps)
{
	unsigned int offset;
	unsigned int masked;
	unsigned int i;
	int err;

	switch (ps->opcode) {
	case HOST1X_OPCODE_GATHER:
		if (ps->last_reg == ps->offset)
			goto non_incr;

		/* fall through */
	case HOST1X_OPCODE_SETCLASS:
	case HOST1X_OPCODE_MASK:
	case HOST1X_OPCODE_INCR:
		i = ps->offset;

		for_each_set_bit_from(i, ps->addr_regs, ps->last_reg + 1) {

			if (ps->mask) {
				offset = i - ps->offset;

				if (!(ps->mask & BIT(offset)))
					continue;

				masked = ps->mask & (BIT(offset) - 1);
				offset = hweight16(masked);
			} else {
				offset = i - ps->offset;
			}

			err = cmdstream_patch_reloc(ps, offset, NULL, NULL,
						    false, false);
			if (err)
				return err;
		}
		break;

	case HOST1X_OPCODE_NONINCR:
non_incr:
		if (test_bit(ps->offset, ps->addr_regs)) {

			for (i = 0; i < ps->count; i++) {
				err = cmdstream_patch_reloc(ps, 0, NULL, NULL,
							    false, false);
				if (err)
					return err;
			}
		}
		break;

	case HOST1X_OPCODE_IMM:
		if (test_bit(ps->offset, ps->addr_regs)) {
			PATCH_ERROR("writing immediate to address register");
			return -EINVAL;
		}
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static inline int cmdstream_update_classid(struct parser_state *ps)
{
	unsigned int refined_class = ps->classid;
	struct tegra_drm_client *drm_client;
	int err;

	if (ps->classid == HOST1X_CLASS_HOST1X) {
		ps->addr_regs = NULL;
		ps->num_regs = 1;

		return 0;
	}

	list_for_each_entry(drm_client, &ps->tegra->clients, list) {
		err = drm_client->refine_class(drm_client, ps->pipes_expected,
					       &refined_class);
		if (err)
			continue;

		if (refined_class != ps->classid) {
			ps->words_in[ps->word_id - 1] &= ~(0x3ff << 6);
			ps->words_in[ps->word_id - 1] |= refined_class << 6;
		}

		ps->drm_client = drm_client;
		ps->addr_regs = drm_client->addr_regs;
		ps->num_regs = drm_client->num_regs;
		ps->pipes |= drm_client->pipe;

		return 0;
	}

	PATCH_ERROR("invalid class id 0x%x", ps->classid);

	return -EINVAL;
}

static inline void
patch_syncpt_incr(struct parser_state *ps, u32 *patch_ptr, u32 data, u32 mask,
		  bool *overflow)
{
	*patch_ptr = data & mask;
	*patch_ptr |= host1x_uclass_incr_syncpt_indx_f(ps->syncpt_id);

	if (ps->syncpt_incrs == 0xffff)
		*overflow = true;

	ps->syncpt_incrs++;
}

static inline int
cmdstream_patch_syncpt_incrs(struct parser_state *ps)
{
	bool overflow = false;
	unsigned int i;

	/* all HW modules have INCR_SYNCPT register at the same offset */
	if (ps->offset > HOST1X_UCLASS_INCR_SYNCPT)
		return 0;

	switch (ps->opcode) {
	case HOST1X_OPCODE_SETCLASS:
	case HOST1X_OPCODE_MASK:
		if (!(ps->mask & BIT(0)))
			return 0;

		/* fall through */
	case HOST1X_OPCODE_INCR:
		patch_syncpt_incr(ps, &ps->words_in[ps->word_id],
				  ps->words_in[ps->word_id],
				  0x0000ff00, &overflow);
		break;

	case HOST1X_OPCODE_NONINCR:
		for (i = 0; i < ps->count; i++)
			patch_syncpt_incr(ps, &ps->words_in[ps->word_id + i],
					  ps->words_in[ps->word_id + i],
					  0x0000ff00, &overflow);
		break;

	case HOST1X_OPCODE_IMM:
		patch_syncpt_incr(ps, &ps->words_in[ps->word_id - 1],
				  ps->words_in[ps->word_id - 1],
				  0xffffff00, &overflow);
		break;

	default:
		return -EINVAL;
	}

	if (overflow) {
		PATCH_ERROR("too many sync point increments");
		return -EINVAL;
	}

	return 0;
}

static inline int cmdstream_patch_gather(struct parser_state *ps)
{
	const struct tegra_bo *bo;
	unsigned int offset;
	int err;

	if (ps->opcode != HOST1X_OPCODE_GATHER)
		return 0;

	err = cmdstream_patch_reloc(ps, 0, &bo, &offset, true, true);
	if (err)
		return err;

	if (offset + ps->count > bo->gem.size) {
		PATCH_ERROR("invalid gather size");
		return -EINVAL;
	}

	return 0;
}

static inline int cmdstream_patch_extend(struct parser_state *ps)
{
	struct drm_tegra_cmdstream_extend_op extend;
	struct tegra_drm_client *drm_client;
	int err = -EINVAL;

	if (ps->opcode != HOST1X_OPCODE_EXTEND)
		return 0;

	extend.u_data = ps->words_in[ps->word_id - 1];

	switch (extend.subop) {
	case HOST1X_OPCODE_EXTEND_ACQUIRE_MLOCK:
	case HOST1X_OPCODE_EXTEND_RELEASE_MLOCK:

		list_for_each_entry(drm_client, &ps->tegra->clients, list) {
			if (drm_client->pipe == 1ull << extend.value) {
				extend.value = drm_client->mlock->id;
				err = 0;
				break;
			}
		}
		break;

	default:
		PATCH_ERROR("invalid extend subop %u", extend.subop);
		return -EINVAL;
	}

	if (err) {
		PATCH_ERROR("invalid extend value %u", extend.value);
		return err;
	}

	ps->words_in[ps->word_id - 1] = extend.u_data;

	return 0;
}

static inline int cmdstream_patch_client(struct parser_state *ps)
{
	int err;

	if (ps->last_reg >= ps->num_regs) {
		PATCH_ERROR("invalid reg address");
		return -EINVAL;
	}

	err = cmdstream_patch_syncpt_incrs(ps);
	if (err)
		return err;

	err = cmdstream_patch_relocs(ps);
	if (err)
		return err;

	err = cmdstream_patch_gather(ps);
	if (err)
		return err;

	return 0;
}

static inline void
patch_syncpt_wait(struct parser_state *ps, u32 *patch_ptr, u32 data)
{
	struct drm_tegra_cmdstream_wait_syncpt wait_syncpt;
	u32 thresh;

	wait_syncpt.u_data = data;

	if (wait_syncpt.threshold)
		thresh = wait_syncpt.threshold;
	else
		thresh = ps->syncpt_incrs;

	*patch_ptr = host1x_class_host_wait_syncpt(ps->syncpt_id, thresh);
}

static inline int
cmdstream_patch_syncpt_waits(struct parser_state *ps)
{
	unsigned long mask;
	unsigned int offset;
	unsigned int i;

	if (ps->offset > HOST1X_UCLASS_WAIT_SYNCPT)
		return 0;

	switch (ps->opcode) {
	case HOST1X_OPCODE_SETCLASS:
	case HOST1X_OPCODE_MASK:
		mask = ps->mask;
		offset = 0;

		for_each_set_bit(i, &mask, 16) {
			if (ps->offset + i == HOST1X_UCLASS_WAIT_SYNCPT)
				goto patch_word;
			offset++;
		}
		break;

	case HOST1X_OPCODE_INCR:
		offset = HOST1X_UCLASS_WAIT_SYNCPT - ps->offset;
patch_word:
		patch_syncpt_wait(ps, &ps->words_in[ps->word_id + offset],
				  ps->words_in[ps->word_id + offset]);
		return 1;

	case HOST1X_OPCODE_NONINCR:
		for (i = 0; i < ps->count; i++)
			patch_syncpt_wait(ps, &ps->words_in[ps->word_id + i],
					  ps->words_in[ps->word_id + i]);
		return ps->count;

	default:
		return -EINVAL;
	}

	return 0;
}

static inline int cmdstream_patch_host1x(struct parser_state *ps)
{
	int ret;

	ret = cmdstream_patch_syncpt_waits(ps);
	if (ret < 0)
		return ret;

	/* all writes must be done only to permitted registers */
	if (ps->count - ret != 0) {
		PATCH_ERROR("writing to restricted register");
		return -EINVAL;
	}

	return 0;
}

static inline int cmdstream_patch(struct parser_state *ps)
{
	int err;

	err = cmdstream_patch_extend(ps);
	if (err)
		return err;

	if (!ps->count && ps->opcode != HOST1X_OPCODE_IMM)
		return 0;

	if (!ps->classid) {
		PATCH_ERROR("classid not selected");
		return -EINVAL;
	}

	if (ps->classid == HOST1X_CLASS_HOST1X)
		return cmdstream_patch_host1x(ps);

	return cmdstream_patch_client(ps);
}

static inline bool cmdstream_proceed(struct parser_state *ps)
{
	if (ps->opcode == HOST1X_OPCODE_GATHER)
		ps->word_id += 1;
	else
		ps->word_id += ps->count;

	if (ps->word_id >= ps->num_words)
		return false;

	return true;
}

static inline int cmdstream_parse_opcode(struct parser_state *ps)
{
	unsigned int words_count;
	u32 word;
	int err;

	word = ps->words_in[ps->word_id++];
	ps->opcode = word >> 28;

	switch (ps->opcode) {
	case HOST1X_OPCODE_SETCLASS:
		ps->offset	= word >> 16 & 0xfff;
		ps->mask	= word & 0x1f;
		ps->count	= hweight8(ps->mask);
		ps->last_reg	= ps->offset + fls(ps->mask) - 1;
		ps->classid	= word >> 6 & 0x3ff;

		err = cmdstream_update_classid(ps);
		if (err)
			return err;

		words_count = ps->count;
		break;

	case HOST1X_OPCODE_INCR:
		ps->offset	= word >> 16 & 0xfff;
		ps->mask	= 0;
		ps->count	= word & 0xffff;
		ps->last_reg	= ps->offset + ps->count - 1;

		words_count = ps->count;
		break;

	case HOST1X_OPCODE_NONINCR:
		ps->offset	= word >> 16 & 0xfff;
		ps->count	= word & 0xffff;
		ps->last_reg	= ps->offset;

		words_count = ps->count;
		break;

	case HOST1X_OPCODE_MASK:
		ps->mask	= word & 0xffff;
		ps->offset	= word >> 16 & 0xfff;
		ps->count	= hweight16(ps->mask);
		ps->last_reg	= ps->offset + fls(ps->mask) - 1;

		words_count = ps->count;
		break;

	case HOST1X_OPCODE_IMM:
		ps->offset	= word >> 16 & 0x1fff;
		ps->count	= 0;
		ps->last_reg	= ps->offset;

		words_count = ps->count;
		break;

	case HOST1X_OPCODE_EXTEND:
		ps->offset	= 0xffff;
		ps->count	= 0;
		ps->last_reg	= 0;

		words_count = ps->count;
		break;

	case HOST1X_OPCODE_GATHER:
		if (!(word & BIT(15))) {
			PATCH_ERROR("only pure data-gather allowed");
			return -EINVAL;
		}

		ps->offset	= word >> 16 & 0xfff;
		ps->count	= word & 0x3fff;
		ps->mask	= 0;

		if (word & BIT(14))
			ps->last_reg = ps->offset + ps->count - 1;
		else
			ps->last_reg = ps->offset;

		words_count = 1;
		break;

	case HOST1X_OPCODE_RESTART:
	case HOST1X_OPCODE_RESTART_W:
	case HOST1X_OPCODE_SETSTRMID:
	case HOST1X_OPCODE_SETAPPID:
	case HOST1X_OPCODE_SETPYLD:
		PATCH_ERROR("forbidden cdma opcode %08x", word);
		return -EINVAL;

	case HOST1X_OPCODE_INCR_W:
	case HOST1X_OPCODE_NONINCR_W:
	case HOST1X_OPCODE_GATHER_W:
		/* unimplemented yet */
		PATCH_ERROR("unsupported cdma opcode %08x", word);
		return -EINVAL;

	default:
		PATCH_ERROR("invalid cdma opcode %08x", word);
		return -EINVAL;
	}

	if (ps->word_id + words_count > ps->num_words) {
		PATCH_ERROR("invalid number of cmdstream words");
		return -EINVAL;
	}

	return 0;
}

int tegra_drm_copy_and_patch_cmdstream(const struct tegra_drm *tegra,
				       struct tegra_drm_job *drm_job,
				       struct tegra_bo *const *bos,
				       u64 pipes_expected,
				       u32 *words_in,
				       u64 *ret_pipes,
				       unsigned int *ret_incrs)
{
	struct host1x_job *job = &drm_job->base;
	unsigned int num_words = job->num_words;

	struct parser_state ps = {
		.drm_job	= drm_job,
		.pipes_expected	= pipes_expected,
		.tegra		= tegra,
		.syncpt_id	= job->syncpt->id,
		.num_bos	= drm_job->num_bos,
		.bos		= bos,
		.addr_regs	= NULL,
		.num_words	= num_words,
		.words_in	= words_in,
		.word_id	= 0,
		.classid	= 0,
		.num_regs	= 0,
		.syncpt_incrs	= 0,
		.pipes		= 0,
	};
	int ret;

	do {
		ret = cmdstream_parse_opcode(&ps);
		if (ret)
			break;

		ret = cmdstream_patch(&ps);
		if (ret)
			break;

	} while (cmdstream_proceed(&ps));

	/* copy the patched commands stream */
	memcpy(job->bo.vaddr, words_in, num_words * sizeof(u32));

	*ret_incrs = ps.syncpt_incrs;
	*ret_pipes = ps.pipes;

	return ret;
}
