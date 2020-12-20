/* SPDX-License-Identifier: GPL-2.0 */

static int host1x_soc_pushbuf_init(struct host1x *host,
				   struct host1x_pushbuf *pb,
				   unsigned int num_words)
{
	unsigned int i;
	u32 *opcodes;

	if (num_words < 8)
		return -EINVAL;

	pb->bo = host1x_bo_alloc(host, num_words * sizeof(u32), true);
	if (!pb->bo)
		return -ENOMEM;

	pb->push_cnt	= 0;
	pb->put_ptr	= pb->bo->vaddr;
	pb->get_ptr	= pb->bo->vaddr;
	pb->start_ptr	= pb->bo->vaddr;
	pb->start_dma	= pb->bo->dmaaddr;

	opcodes = pb->bo->vaddr;

	/* pre-fill push buffer with NOP's to ease debugging a tad */
	for (i = 0; i < num_words; i++)
		opcodes[i] = HOST1X_OPCODE_NOP;

	/* set up wraparound (restart) address to create a ring buffer */
#if HOST1X_HW < 6
	pb->words = num_words - 1;

	opcodes[pb->words] = host1x_opcode_restart(pb->bo->dmaaddr);
#else
	pb->words = num_words - 3;

	opcodes[pb->words + 0] = HOST1X_OPCODE_RESTART_W << 28;
	opcodes[pb->words + 1] = lower_32_bits(pb->bo->dmaaddr);
	opcodes[pb->words + 2] = upper_32_bits(pb->bo->dmaaddr);
#endif
	/* push buffer data mapping is write-combined */
	wmb();

	spin_lock_init(&pb->lock);

	return 0;
}

static void host1x_soc_pushbuf_release(struct host1x *host,
				       struct host1x_pushbuf *pb)
{
	host1x_bo_free(host, pb->bo);
}

static inline u32 *
host1x_soc_pushbuf_start_ptr(struct host1x_pushbuf *pb)
{
	return pb->start_ptr;
}

static inline u32 *
host1x_soc_pushbuf_end_ptr(struct host1x_pushbuf *pb)
{
	return pb->start_ptr + pb->words;
}

static inline dma_addr_t
host1x_soc_pushbuf_dmastart(struct host1x_pushbuf *pb)
{
	return pb->start_dma;
}

static inline unsigned int
host1x_soc_pushbuf_put_offset(struct host1x_pushbuf *pb)
{
	return (ptrdiff_t)pb->put_ptr - (ptrdiff_t)pb->start_ptr;
}

static inline void
host1x_soc_pushbuf_push(struct host1x_pushbuf *pb, u32 word)
{
	*pb->put_ptr++ = word;
	pb->push_cnt++;

	WARN_ON_ONCE(pb->push_cnt > pb->words);

	if (pb->put_ptr == host1x_soc_pushbuf_end_ptr(pb))
		pb->put_ptr -= pb->words;
}

static inline
void host1x_soc_pushbuf_pop(struct host1x_pushbuf *pb, unsigned int num_words)
{
	pb->get_ptr += num_words;
	pb->push_cnt -= num_words;

	WARN_ON_ONCE(pb->push_cnt < 0);

	if (pb->get_ptr >= host1x_soc_pushbuf_end_ptr(pb))
		pb->get_ptr -= pb->words;
}

static inline dma_addr_t
host1x_soc_pushbuf_dmaput_addr(struct host1x_pushbuf *pb)
{
	ptrdiff_t offset = pb->put_ptr - host1x_soc_pushbuf_start_ptr(pb);

	return pb->start_dma + offset * sizeof(u32);
}

static inline dma_addr_t
host1x_soc_pushbuf_dmaend_addr(struct host1x_pushbuf *pb)
{
	/*
	 * Note that this excludes the RESTART opcode at the end of
	 * push buffer.
	 */
	return pb->start_dma + pb->words * sizeof(u32);
}

static inline
int host1x_soc_pushbuf_align(struct host1x_pushbuf *pb, unsigned int align)
{
	dma_addr_t put = host1x_soc_pushbuf_dmaput_addr(pb) >> 2;
	dma_addr_t end = host1x_soc_pushbuf_dmaend_addr(pb) >> 2;
	unsigned int pushes;
	unsigned int i;

	pushes = round_up(put, align >> 2) - put;
	if (pushes) {
		if (put + pushes >= end)
			pushes = end - put;

		/*
		 * We could jump over these pushes, but then the uninitialized
		 * words will contain garbage and push buffer debug dumping
		 * won't work well.
		 */
		for (i = 0; i < pushes; i++)
			host1x_soc_pushbuf_push(pb, HOST1X_OPCODE_NOP);
	}

	return pushes;
}

static inline unsigned int
host1x_soc_push_return_from_job(struct host1x_pushbuf *pb,
				struct host1x_job *job)
{
	u32 *cmds = job->bo.vaddr;
	dma_addr_t restart_addr;
	unsigned int pushes;

	/* return address must be aligned to 16 bytes */
	pushes		= host1x_soc_pushbuf_align(pb, 16);
	restart_addr	= host1x_soc_pushbuf_dmaput_addr(pb);

	WARN_ON_ONCE(job->bo.size == job->num_words * sizeof(u32));

	/*
	 * Add CDMA restart command to the job's commands buffer that
	 * returns CDMA to the push buffer.
	 */
#if HOST1X_HW < 6
	cmds[job->num_words++] = host1x_opcode_restart(restart_addr);
#else
	cmds[job->num_words++] = HOST1X_OPCODE_RESTART_W << 28;
	cmds[job->num_words++] = lower_32_bits(restart_addr);
	cmds[job->num_words++] = upper_32_bits(restart_addr);
#endif

	return pushes;
}

static inline unsigned int
host1x_soc_pushbuf_prepare(struct host1x_pushbuf *pb, unsigned num_pushes)
{
	unsigned int pushes = 0;

	/* make sure that next few pushes are contiguous in push buffer */
	if (pb->put_ptr + num_pushes > host1x_soc_pushbuf_end_ptr(pb)) {
		do {
			host1x_soc_pushbuf_push(pb, HOST1X_OPCODE_NOP);
			pushes++;
		} while (pb->put_ptr != host1x_soc_pushbuf_start_ptr(pb));
	}

	return pushes;
}

static inline unsigned int
host1x_soc_pushbuf_push_incr_and_wait(struct host1x_pushbuf *pb,
				      struct host1x_job *job)
{
	unsigned int pushes = 3;

	u32 op1 = host1x_opcode_imm_incr_syncpt(HOST1X_SYNCPT_COND_IMMEDIATE,
						job->syncpt->id);

	u32 op2 = host1x_opcode_setclass(HOST1X_CLASS_HOST1X,
					 HOST1X_UCLASS_WAIT_SYNCPT,
					 0x1);

	u32 method = host1x_class_host_wait_syncpt(job->syncpt->id,
						   job->num_incrs + 1);

	host1x_soc_pushbuf_push(pb, op1);

	pushes += host1x_soc_pushbuf_prepare(pb, 2);

	host1x_soc_pushbuf_push(pb, op2);
	host1x_soc_pushbuf_push(pb, method);

	return pushes;
}

static inline unsigned int
host1x_soc_push_job(struct host1x_pushbuf *pb, struct host1x_job *job)
{
#if HOST1X_HW < 6
	host1x_soc_pushbuf_push(pb, host1x_opcode_restart(job->bo.dmaaddr));

	return 1;
#else
#if IS_ENABLED(CONFIG_IOMMU_API)
	struct iommu_fwspec *spec = dev_iommu_fwspec_get(job->chan->host->dev);
	u32 sid = spec ? spec->ids[0] & 0xffff : 0x7f;
#else
	u32 sid = 0x7f;
#endif
	unsigned int pushes = 5;
	u32 op1, op2;

	op1 = host1x_opcode_imm(HOST1X_OPCODE_SETSTRMID, sid);
	op2 = host1x_opcode_imm(HOST1X_OPCODE_SETAPPID, job->syncpt->id);

	host1x_soc_pushbuf_push(pb, op1);
	host1x_soc_pushbuf_push(pb, op2);

	pushes += host1x_soc_pushbuf_prepare(pb, 3);

	host1x_soc_pushbuf_push(pb, HOST1X_OPCODE_RESTART_W << 28);
	host1x_soc_pushbuf_push(pb, lower_32_bits(job->bo.dmaaddr));
	host1x_soc_pushbuf_push(pb, upper_32_bits(job->bo.dmaaddr));

	return pushes;
#endif
}

static inline unsigned int
host1x_soc_push_init_gather(struct host1x_pushbuf *pb, struct host1x_gather *g)
{
	unsigned int pushes = 2;

	pushes += host1x_soc_pushbuf_prepare(pb, 2);

	host1x_soc_pushbuf_push(pb, host1x_opcode_gather(g->num_words));
	host1x_soc_pushbuf_push(pb, g->bo->dmaaddr);

	return pushes;
}

static inline void
host1x_soc_pushbuf_push_job(struct host1x_pushbuf *pb,
			    struct host1x_job *job)
{
	unsigned int pushes = 0;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&pb->lock, flags);

	/*
	 * Job's execution flow:
	 *	pb -> job.init_gather (optional)
	 *		-> job.start_addr
	 *			-> pb.ret_addr
	 *				-> incr_sp
	 *					-> done
	 */
	for (i = 0; i < job->num_init_gathers; i++)
		pushes += host1x_soc_push_init_gather(pb, job->init_gathers[i]);

	pushes += host1x_soc_push_job(pb, job);
	pushes += host1x_soc_push_return_from_job(pb, job);
	pushes += host1x_soc_pushbuf_push_incr_and_wait(pb, job);

	job->num_pb_pushes = pushes;

	spin_unlock_irqrestore(&pb->lock, flags);
}

static inline void
host1x_soc_pushbuf_pop_job(struct host1x_pushbuf *pb, struct host1x_job *job)
{
	unsigned long flags;
	unsigned int pushes;

	spin_lock_irqsave(&pb->lock, flags);

	pushes = job->num_pb_pushes;
	if (pushes) {
		host1x_soc_pushbuf_pop(pb, pushes);
		job->num_pb_pushes = 0;
	}

	spin_unlock_irqrestore(&pb->lock, flags);
}
