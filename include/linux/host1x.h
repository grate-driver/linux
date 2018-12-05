/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2009-2013, NVIDIA Corporation. All rights reserved.
 */

#ifndef __LINUX_HOST1X_H
#define __LINUX_HOST1X_H

#include <linux/atomic.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/genalloc.h>
#include <linux/idr.h>
#include <linux/iommu.h>
#include <linux/iova.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/**
 * enum host1x_class - host1x class ID's
 *
 * Enumeration of host1x class ID's.
 */
enum host1x_class {
	HOST1X_CLASS_HOST1X		= 0x1,
	HOST1X_CLASS_GR2D_G2_0_CTX1	= 0x50,
	HOST1X_CLASS_GR2D_G2_0_CTX2	= 0x54,
	HOST1X_CLASS_GR2D_G2_0_CTX3	= 0x58,
	HOST1X_CLASS_GR2D_G2_1_CTX1	= 0x51,
	HOST1X_CLASS_GR2D_G2_1_CTX2	= 0x55,
	HOST1X_CLASS_GR2D_SB_CTX1	= 0x52,
	HOST1X_CLASS_GR2D_SB_CTX2	= 0x56,
	HOST1X_CLASS_GR2D_SB_CTX3	= 0x5a,
	HOST1X_CLASS_VIC		= 0x5D,
	HOST1X_CLASS_GR3D		= 0x60,
};

/**
 * enum host1x_module - host1x module ID's
 *
 * Enumeration of host1x module ID's.
 */
enum host1x_module {
	HOST1X_MODULE_HOST1X		= 0,
	HOST1X_MODULE_GR2D		= 5,
	HOST1X_MODULE_GR3D		= 6,
	HOST1X_MODULE_VIC		= 13,
};

/**
 * enum host1x_opcode - host1x opcodes
 *
 * Enumeration of host1x channel DMA opcodes.
 */
enum host1x_opcode {
	HOST1X_OPCODE_SETCLASS		= 0x00,
	HOST1X_OPCODE_INCR		= 0x01,
	HOST1X_OPCODE_NONINCR		= 0x02,
	HOST1X_OPCODE_MASK		= 0x03,
	HOST1X_OPCODE_IMM		= 0x04,
	HOST1X_OPCODE_RESTART		= 0x05,
	HOST1X_OPCODE_GATHER		= 0x06,
	HOST1X_OPCODE_SETSTRMID		= 0x07,
	HOST1X_OPCODE_SETAPPID		= 0x08,
	HOST1X_OPCODE_SETPYLD		= 0x09,
	HOST1X_OPCODE_INCR_W		= 0x0a,
	HOST1X_OPCODE_NONINCR_W		= 0x0b,
	HOST1X_OPCODE_GATHER_W		= 0x0c,
	HOST1X_OPCODE_RESTART_W 	= 0x0d,
	HOST1X_OPCODE_EXTEND		= 0x0e,
};

/**
 * enum host1x_opcode_extend - host1x "extended" opcode
 *
 * Enumeration of host1x "extended" opcode sub-operations.
 */
enum host1x_opcode_extend {
	HOST1X_OPCODE_EXTEND_ACQUIRE_MLOCK	= 0x00,
	HOST1X_OPCODE_EXTEND_RELEASE_MLOCK	= 0x01,
};

/**
 * enum host1x_syncpt_cond - host1x sync point conditions
 *
 * Enumeration of host1x sync point conditions that specify when sync point
 * value shall be incremented.
 */
enum host1x_syncpt_cond {
	HOST1X_SYNCPT_COND_IMMEDIATE	= 0x00,
	HOST1X_SYNCPT_COND_OP_DONE	= 0x01,
	HOST1X_SYNCPT_COND_RD_DONE	= 0x02,
	HOST1X_SYNCPT_COND_REG_WR_SAFE	= 0x03,
};

struct host1x;
struct host1x_dbg_output;
struct host1x_channel;
struct host1x_job;
struct host1x_syncpt;
struct host1x_pushbuf;
struct host1x_bo;

/**
 * struct host1x_soc_channel_ops - host1x channel operations
 */
struct host1x_soc_channel_ops {
	/**
	 * @init:
	 *
	 * Hook for channels initialization done on host1x driver load.
	 */
	int (*init)(struct host1x *host);

	/**
	 * @deinit:
	 *
	 * Hook for channels de-initialization done on host1x driver unload.
	 */
	void (*deinit)(struct host1x *host);

	/**
	 * @request:
	 *
	 * Hook to allocate and return one channel. Returned channel will have
	 * push buffer of a specified size given in words.
	 */
	struct host1x_channel * (*request)(struct host1x *host,
					   struct device *dev,
					   unsigned int num_pushbuf_words);

	/**
	 * @release:
	 *
	 * Hook for releasing/freeing a requested channel.
	 */
	void (*release)(struct kref *kref);

	/**
	 * @reset:
	 *
	 * Hook for resetting channels hardware.
	 */
	void (*reset)(struct host1x_channel *chan);

	/**
	 * @submit:
	 *
	 * Hook for submitting job into channel.
	 */
	struct dma_fence * (*submit)(struct host1x_channel *chan,
				     struct host1x_job *job,
				     struct dma_fence *fence);

	/**
	 * @cleanup_job:
	 *
	 * Hook for cleaning up state of a hung job.
	 */
	void (*cleanup_job)(struct host1x_channel *chan,
			    struct host1x_job *job,
			    struct dma_fence *fence);

	/**
	 * @dmaget:
	 *
	 * Hook for getting channels hardware DMAGET address.
	 */
	dma_addr_t (*dmaget)(struct host1x_channel *chan);
};

/**
 * struct host1x_soc_syncpt_ops - host1x sync point operations
 */
struct host1x_soc_syncpt_ops {
	/**
	 * @init:
	 *
	 * Hook for sync points initialization done on host1x driver load.
	 */
	int (*init)(struct host1x *host);

	/**
	 * @deinit:
	 *
	 * Hook for sync points de-initialization done on host1x driver unload.
	 */
	void (*deinit)(struct host1x *host);

	/**
	 * @request:
	 *
	 * Hook to allocate and return one sync point.
	 */
	struct host1x_syncpt * (*request)(struct host1x *host);

	/**
	 * @release:
	 *
	 * Hook for releasing allocated sync point.
	 */
	void (*release)(struct kref *kref);

	/**
	 * @reset:
	 *
	 * Hook for resetting sync point hardware.
	 */
	void (*reset)(struct host1x_syncpt *syncpt, int error);

	/**
	 * @set_interrupt:
	 *
	 * Hook for sync point hardware interrupt enabling / disabling.
	 */
	void (*set_interrupt)(struct host1x_syncpt *syncpt, bool enabled);

	/**
	 * @read:
	 *
	 * Hook for reading sync point hardware counter value.
	 */
	u32 (*read)(struct host1x_syncpt *syncpt);

	/**
	 * @detach_fences:
	 *
	 * Hook for detaching fences from sync point without signaling them.
	 */
	void (*detach_fences)(struct host1x_syncpt *syncpt);
};

/**
 * struct host1x_soc_mlock_ops - host1x MLOCK operations
 */
struct host1x_soc_mlock_ops {
	/**
	 * @init:
	 *
	 * Hook for MLOCK's initialization done on host1x driver load.
	 */
	int (*init)(struct host1x *host);

	/**
	 * @deinit:
	 *
	 * Hook for MLOCK's de-initialization done on host1x driver unload.
	 */
	void (*deinit)(struct host1x *host);

	/**
	 * @request:
	 *
	 * Hook to allocate and return one MLOCK.
	 */
	struct host1x_mlock * (*request)(struct host1x *host,
					 struct device *dev);

	/**
	 * @release:
	 *
	 * Hook for releasing allocated MLOCK.
	 */
	void (*release)(struct kref *kref);

	/**
	 * @unlock_channel:
	 *
	 * Hook to unlock all MLOCKs held by given channel.
	 */
	void (*unlock_channel)(struct host1x_channel *chan);
};

/**
 * struct host1x_soc_syncpt_ops - host1x sync point operations
 */
struct host1x_soc_dbg_ops {
	/**
	 * @dump_cmdbuf:
	 *
	 * Hook for parsing and printing out CDMA commands stream.
	 */
	void (*dump_cmdbuf)(struct host1x_dbg_output *o,
			    struct host1x_bo *bo, unsigned int num_words);

	/**
	 * @dump_syncpt:
	 *
	 * Hook for printing out sync point hardware state.
	 */
	void (*dump_syncpt)(struct host1x_dbg_output *o,
			    struct host1x_syncpt *syncpt);

	/**
	 * @dump_syncpts:
	 *
	 * Hook for printing out hardware state of all sync points.
	 */
	void (*dump_syncpts)(struct host1x_dbg_output *o,
			     struct host1x *host);

	/**
	 * @dump_channel:
	 *
	 * Hook for printing out channels hardware state.
	 */
	void (*dump_channel)(struct host1x_dbg_output *o,
			     struct host1x_channel *chan);

	/**
	 * @dump_channels:
	 *
	 * Hook for printing out hardware state of all channels.
	 */
	void (*dump_channels)(struct host1x_dbg_output *o,
			      struct host1x *host);

	/**
	 * @dump_mlocks:
	 *
	 * Hook for printing out mocks hardware state.
	 */
	void (*dump_mlocks)(struct host1x_dbg_output *o,
			    struct host1x *host);
};

/**
 * struct host1x_sid_entry - host1x SMMU Stream ID entry
 * @base: SID host1x register address
 * @offset: client's base address
 * @limit: client's limit address
 */
struct host1x_sid_entry {
	unsigned int base;
	unsigned int offset;
	unsigned int limit;
};

/**
 * struct host1x_soc - host1x SoC-specific features
 * @dma_mask: mask of addressable memory
 * @has_hypervisor: SoC has hypervisor registers
 * @nb_channels: number of channels supported
 * @nb_syncpts: number of sync points supported
 * @nb_bases: number of sync point bases supported
 * @nb_mlocks: number of mlocks supported
 * @nb_sid_entries: number of Stream ID entries
 * @sid_table: pointer to Stream ID table
 * @init_ops: hook for setting up SoC-specific channel / sync point / debug ops
 */
struct host1x_soc {
	u64 dma_mask;
	bool has_hypervisor;
	unsigned int nb_channels;
	unsigned int nb_syncpts;
	unsigned int nb_bases;
	unsigned int nb_mlocks;
	unsigned int nb_sid_entries;
	const struct host1x_sid_entry *sid_table;
	int (*init_ops)(struct host1x *host);
};

/**
 * struct host1x - host1x device structure
 */
struct host1x {
	const struct host1x_soc *soc;
	struct host1x_soc_channel_ops chan_ops;
	struct host1x_soc_syncpt_ops syncpt_ops;
	struct host1x_soc_mlock_ops mlock_ops;
	struct host1x_soc_dbg_ops dbg_ops;
	void __iomem *hv_regs;
	void __iomem *base_regs;
	unsigned long *active_syncpts;
	struct idr syncpts;
	struct kmem_cache *syncpts_slab;
	struct completion syncpt_release_complete;
	struct list_head pool_chunks;
	struct gen_pool *pool;
	struct iommu_group *group;
	struct iommu_domain *domain;
	struct iova_domain iova;
	dma_addr_t iova_end;
	struct reset_control *rst;
	struct device *dev;
	struct clk *clk;
	struct dentry *debugfs;
	struct mutex devices_lock;
	struct list_head devices;
	struct list_head list;
	struct idr channels;
	spinlock_t channels_lock;
	struct idr mlocks;
	spinlock_t mlocks_lock;
	atomic_t fence_seq;
	int syncpt_irq;
	spinlock_t debug_lock;
};

/**
 * struct host1x_syncpt - host1x sync point
 */
struct host1x_syncpt {
	/**
	 * @id:
	 *
	 * Hardware ID of sync point.
	 */
	unsigned int id;

	/**
	 * @fences:
	 *
	 * List of attached dma_fance's.
	 */
	struct list_head fences;

	/**
	 * @refcount:
	 *
	 * Sync point refcounting.
	 */
	struct kref refcount;

	/**
	 * @host:
	 *
	 * Pointer to @host1x structure.
	 */
	struct host1x *host;

	/**
	 * @dev:
	 *
	 * Pointer to device that requested sync point. Could be NULL.
	 */
	struct device *dev;
};

/**
 * struct host1x_fence - host1x fence
 */
struct host1x_fence {
	/**
	 * @base:
	 *
	 * dma_fence backing structure.
	 */
	struct dma_fence base;

	/**
	 * @syncpt_thresh:
	 *
	 * Sync point HW threshold value.
	 */
	u32 syncpt_thresh;

	/**
	 * @list:
	 *
	 * Node of @host1x_syncpt fences list.
	 */
	struct list_head list;

	/**
	 * @channel:
	 *
	 * Pointer to @host1x_channel structure.
	 */
	struct host1x_channel *channel;
};

/**
 * struct host1x_bo - host1x buffer object
 */
struct host1x_bo {
	/**
	 * @addr:
	 *
	 * Buffer object physical / DMA address.
	 */
	dma_addr_t addr;

	/**
	 * @dmaaddr:
	 *
	 * Buffer object CDMA address.
	 */
	dma_addr_t dmaaddr;

	/**
	 * @vaddr:
	 *
	 * Buffer object virtual address.
	 */
	void *vaddr;

	/**
	 * @size:
	 *
	 * Buffer object size in bytes.
	 */
	size_t size;

	/**
	 * @dma_attrs:
	 *
	 * Bitmask of DMA API allocation attributes (DMA_ATTR_*).
	 */
	unsigned long dma_attrs;

	/**
	 * @from_pool:
	 *
	 * Buffer object allocated from gen_pool.
	 */
	bool from_pool : 1;
};

/**
 * struct host1x_bo - host1x push buffer
 *
 * Channel DMA ring buffer to which jobs are "pushed" and "popped" from.
 */
struct host1x_pushbuf {
	/**
	 * @bo:
	 *
	 * Pointer to backing buffer object.
	 */
	struct host1x_bo *bo;

	/**
	 * @lock:
	 *
	 * Lock to protect from simultaneous pushing from different threads.
	 */
	spinlock_t lock;

	/**
	 * @start_dma:
	 *
	 * DMA address that points to the start of "push buffer".
	 */
	dma_addr_t start_dma;

	/**
	 * @start_ptr:
	 *
	 * Virtual address that points to the start of "push buffer".
	 */
	u32 *start_ptr;

	/**
	 * @put_ptr:
	 *
	 * Virtual address that points to the execution end (put) address
	 * within push buffer. It is incremented when job is "pushed" to push
	 * buffer.
	 */
	u32 *put_ptr;

	/**
	 * @get_ptr:
	 *
	 * Virtual address that points to the execution start (get) address
	 * within push buffer. It is incremented when job is "popped" from push
	 * buffer.
	 */
	u32 *get_ptr;

	/**
	 * @push_cnt:
	 *
	 * Number of pushes done into push buffer. Incremented on "push" and
	 * decremented on "pop". Used solely for debugging purposes ("get" must
	 * not cross "put" and vice versa).
	 */
	int push_cnt;

	/**
	 * @words:
	 *
	 * Maximum number of U32 words that can be pushed into push buffer
	 * without overflowing ring buffer.
	 */
	int words;
};

/**
 * struct host1x_channel - host1x channel
 */
struct host1x_channel {
	/**
	 * @refcount:
	 *
	 * Channel refcounting.
	 */
	struct kref refcount;

	/**
	 * @host:
	 *
	 * Pointer to @host1x structure.
	 */
	struct host1x *host;

	/**
	 * @pb:
	 *
	 * @host1x_pushbuf of the channel.
	 */
	struct host1x_pushbuf pb;

	/**
	 * @id:
	 *
	 * Hardware ID of the channel.
	 */
	unsigned int id;

	/**
	 * @dev:
	 *
	 * Pointer to device that requested channel. Could be NULL.
	 */
	struct device *dev;
};

/**
 * struct host1x_mlock - host1x module lock
 */
struct host1x_mlock {
	/**
	 * @id:
	 *
	 * Hardware ID of the MLOCK.
	 */
	unsigned int id;

	/**
	 * @refcount:
	 *
	 * MLOCK refcounting.
	 */
	struct kref refcount;

	/**
	 * @host:
	 *
	 * Pointer to @host1x structure.
	 */
	struct host1x *host;

	/**
	 * @dev:
	 *
	 * Pointer to device that requested MLOCK. Could be NULL.
	 */
	struct device *dev;
};

/**
 * struct host1x_pool_entry - host1x DMA pool entry
 *
 * Describes allocated memory area within DMA pool (gen_pool). It backs
 * @host1x_bo if BO memory is allocated from DMA pool.
 */
struct host1x_pool_entry {
	/**
	 * @list:
	 *
	 * Node of @host1x pool_chunks list.
	 */
	struct list_head list;

	/**
	 * @dmaaddr:
	 *
	 * DMA address of the memory area.
	 */
	dma_addr_t dmaaddr;

	/**
	 * @addr:
	 *
	 * Physical or DMA address of the memory area, depends on kernel's
	 * configuration. For internal use.
	 */
	dma_addr_t addr;

	/**
	 * @vaddr:
	 *
	 * Virtual address of the memory area.
	 */
	void *vaddr;

	/**
	 * @size:
	 *
	 * Memory area size in bytes.
	 */
	size_t size;

	/**
	 * @dma_attrs:
	 *
	 * Bitmask of DMA API allocation attributes (DMA_ATTR_*).
	 */
	unsigned long dma_attrs;
};

/**
 * struct host1x_gather - host1x gather
 *
 * Wrapper around @host1x_bo that represents a "host1x gather".
 */
struct host1x_gather {
	/**
	 * @bo:
	 *
	 * @host1x_bo that contains gather data.
	 */
	struct host1x_bo *bo;

	/**
	 * @num_words:
	 *
	 * Number of words contained within @bo.
	 */
	unsigned int num_words;
};

/**
 * struct host1x_job - host1x job
 */
struct host1x_job {
	/**
	 * @bo:
	 *
	 * @host1x_bo that contains CDMA commands.
	 */
	struct host1x_bo bo;

	/**
	 * @init_gathers:
	 *
	 * @host1x_gather that contains CDMA commands to be executed first.
	 * Used to initialize HW state before userspace job is executed.
	 */
	struct host1x_gather *init_gathers[2];

	/**
	 * @num_init_gathers:
	 *
	 * Number of gathers contained within @init_gathers.
	 */
	unsigned int num_init_gathers;

	/**
	 * @cb:
	 *
	 * Callback that is invoked (in interrupt context) when job execution
	 * completes.
	 */
	struct dma_fence_cb cb;

	/**
	 * @chan:
	 *
	 * @host1x_channel to which job is submitted.
	 */
	struct host1x_channel *chan;

	/**
	 * @syncpt:
	 *
	 * @host1x_syncpt associated with the job.
	 */
	struct host1x_syncpt *syncpt;

	/**
	 * @num_incrs:
	 *
	 * Number of @syncpt increments done by the job.
	 */
	unsigned int num_incrs;

	/**
	 * @num_words:
	 *
	 * Number of CDMA commands within @bo.
	 */
	unsigned int num_words;

	/**
	 * @num_pb_pushes:
	 *
	 * Number of pushes to @chan push buffer caused by submitting the job.
	 */
	unsigned int num_pb_pushes;

	/**
	 * @context:
	 *
	 * dma_fence context for the job.
	 */
	u64 context;
};

/**
 * struct host1x_dbg_output - host1x debug output
 */
struct host1x_dbg_output {
	/**
	 * @fn:
	 *
	 * Hook for printing out debug message.
	 */
	void (*fn)(const char *str, size_t len, bool cont, void *opaque);

	/**
	 * @opaque:
	 *
	 * Private field.
	 */
	void *opaque;

	/**
	 * @buf:
	 *
	 * For internal use.
	 */
	char buf[256];
};

/* Host1x MLOCK API */

/**
 * host1x_mlock_request - allocate MLOCK
 * @host: pointer to @host1x
 * @dev: device that performs the request, could be NULL
 *
 * Returns allocated MLOCK on success or ERR_PTR.
 */
static inline struct host1x_mlock *
host1x_mlock_request(struct host1x *host, struct device *dev)
{
	return host->mlock_ops.request(host, dev);
}

/**
 * host1x_mlock_get - bump refcount
 * @mlock: pointer to @host1x_mlock to ref, can be NULL
 *
 * Returns @mlock.
 */
static inline struct host1x_mlock *
host1x_mlock_get(struct host1x_mlock *mlock)
{
	if (mlock)
		kref_get(&mlock->refcount);

	return mlock;
}

/**
 * host1x_mlock_put - drop refcount
 * @mlock: pointer to @host1x_mlock to unref, can be NULL
 *
 * MLOCK is released when refcount drops to 0.
 */
static inline void
host1x_mlock_put(struct host1x_mlock *mlock)
{
	if (mlock)
		kref_put(&mlock->refcount, mlock->host->mlock_ops.release);
}

/**
 * host1x_unlock_channel_mlocks - unlock all MLOCKs held by channel
 * @chan: pointer to @host1x_channel
 *
 * All MLOCKs held by @chan are unlocked.
 */
static inline void
host1x_unlock_channel_mlocks(struct host1x_channel *chan)
{
	chan->host->mlock_ops.unlock_channel(chan);
}

/* Host1x Fence API */

/**
 * host1x_fence_create - create host1x DMA fence
 * @chan: channel associated with @syncpt
 * @syncpt: sync point
 * @threshold: sync point threshold value
 * @context: DMA fence context
 *
 * Fence is signalled when @syncpt counter is equal or higher than @threshold
 * value.
 *
 * Returns created fence on success, NULL otherwise.
 */
struct dma_fence *host1x_fence_create(struct host1x_channel *chan,
				      struct host1x_syncpt *syncpt,
				      u32 threshold, u64 context);

extern const struct dma_fence_ops host1x_fence_ops;

static inline struct host1x_fence *
to_host1x_fence(struct dma_fence *f)
{
	if (f && f->ops == &host1x_fence_ops)
		return container_of(f, struct host1x_fence, base);

	return NULL;
}

/* Host1x DMA pool API */

/**
 * host1x_dma_pool_grow - grow DMA pool
 * @host: pointer to @host1x
 * @size: size to grow by
 *
 * Reserves more memory for the pool.
 *
 * Returns 0 on success, errno otherwise.
 */
int host1x_dma_pool_grow(struct host1x *host, size_t size);

/* Host1x Debug API */

/**
 * host1x_debug_output - print debug message into given output
 * @o: pointer to @host1x_dbg_output
 * @fmt: string formatting
 */
void host1x_debug_output(struct host1x_dbg_output *o, const char *fmt, ...);

/**
 * host1x_debug_cont - print debug message into given output without line break
 * @o: pointer to @host1x_dbg_output
 * @fmt: string formatting
 */
void host1x_debug_cont(struct host1x_dbg_output *o, const char *fmt, ...);

/**
 * host1x_debug_output_lock - lock debug output
 * @host: pointer to @host1x
 */
static inline void host1x_debug_output_lock(struct host1x *host)
{
	spin_lock(&host->debug_lock);
}

/**
 * host1x_debug_output_unlock - unlock debug output
 * @host: pointer to @host1x
 */
static inline void host1x_debug_output_unlock(struct host1x *host)
{
	spin_unlock(&host->debug_lock);
}

/**
 * host1x_debug_dump_cmdbuf - show CDMA opcodes and data
 * @host: pointer to @host1x
 * @o: pointer to @host1x_dbg_output
 * @bo: pointer to @host1x_bo to dump
 * @num_words: number of words to parse
 *
 * Parses and prints out @bo commands stream until @num_words is parsed.
 */
static inline void
host1x_debug_dump_cmdbuf(struct host1x *host,
			 struct host1x_dbg_output *o,
			 struct host1x_bo *bo,
			 unsigned int num_words)
{
	host->dbg_ops.dump_cmdbuf(o, bo, num_words);
}

/**
 * host1x_debug_dump_syncpt - show one sync point state
 * @host: pointer to @host1x
 * @o: pointer to @host1x_dbg_output
 * @syncpt: pointer to @host1x_syncpt to dump
 *
 * Prints out @syncpt hardware state.
 */
static inline void
host1x_debug_dump_syncpt(struct host1x *host,
			 struct host1x_dbg_output *o,
			 struct host1x_syncpt *syncpt)
{
	host->dbg_ops.dump_syncpt(o, syncpt);
}

/**
 * host1x_debug_dump_syncpts - show state of all sync points
 * @host: pointer to @host1x
 * @o: pointer to @host1x_dbg_output
 *
 * Prints out hardware state of all sync points.
 */
static inline void
host1x_debug_dump_syncpts(struct host1x *host,
			  struct host1x_dbg_output *o)
{
	host1x_debug_output(o, "sync points dump:\n");
	host->dbg_ops.dump_syncpts(o, host);
}

/**
 * host1x_debug_dump_channel - show one channel state
 * @host: pointer to @host1x
 * @o: pointer to @host1x_dbg_output
 * @chan: pointer to @host1x_channel to dump
 *
 * Prints out @chan hardware state.
 */
static inline void
host1x_debug_dump_channel(struct host1x *host,
			  struct host1x_dbg_output *o,
			  struct host1x_channel *chan)
{
	host->dbg_ops.dump_channel(o, chan);
}

/**
 * host1x_debug_dump_channels - show state of all channels
 * @host: pointer to @host1x
 * @o: pointer to @host1x_dbg_output
 *
 * Prints out hardware state of all channels.
 */
static inline void
host1x_debug_dump_channels(struct host1x *host,
			   struct host1x_dbg_output *o)
{
	host1x_debug_output(o, "channels dump:\n");
	host->dbg_ops.dump_channels(o, host);
}

/**
 * host1x_debug_dump_mlocks - show MLOCKs state
 * @host: pointer to @host1x
 * @o: pointer to @host1x_dbg_output
 *
 * Prints out MLOCKs hardware state.
 */
static inline void
host1x_debug_dump_mlocks(struct host1x *host,
			 struct host1x_dbg_output *o)
{
	host1x_debug_output(o, "mlocks dump:\n");
	host->dbg_ops.dump_mlocks(o, host);
}

/**
 * host1x_debug_dump_job - show job's CDMA opcodes and data
 * @host: pointer to @host1x
 * @o: pointer to @host1x_dbg_output
 * @job: pointer to @host1x_job to dump
 *
 * Parses and prints out commands stream of the @job.
 */
static inline void
host1x_debug_dump_job(struct host1x *host,
		      struct host1x_dbg_output *o,
		      struct host1x_job *job)
{
	struct host1x_gather *g;
	unsigned int i;

	for (i = 0; i < job->num_init_gathers; i++) {
		g = job->init_gathers[i];

		host1x_debug_output(o, "job init-gather %u dump:\n", i);
		host1x_debug_dump_cmdbuf(host, o, g->bo, g->num_words);
	}

	host1x_debug_output(o, "job cmdstream dump:\n");
	host1x_debug_dump_cmdbuf(host, o, &job->bo, job->num_words);
}

/**
 * host1x_debug_dump_channels_pushbuf - show channel's push buffer CDMA opcodes
 * @host: pointer to @host1x
 * @o: pointer to @host1x_dbg_output
 * @chan: pointer to @host1x_channel to dump
 *
 * Parses and prints out commands stream within @chan push buffer ring.
 */
static inline void
host1x_debug_dump_channels_pushbuf(struct host1x *host,
				   struct host1x_dbg_output *o,
				   struct host1x_channel *chan)
{
	host1x_debug_output(o, "pushbuf dump:\n");
	host1x_debug_dump_cmdbuf(host, o, chan->pb.bo, chan->pb.words + 1);
}

/* Host1x Sync Point API */

extern spinlock_t host1x_syncpts_lock;

/**
 * host1x_syncpt_request - allocate sync point
 * @host: pointer to @host1x
 *
 * Returns allocated sync point on success or ERR_PTR. Blocks until
 * sync point ID is available.
 */
static inline struct host1x_syncpt *
host1x_syncpt_request(struct host1x *host)
{
	return host->syncpt_ops.request(host);
}

/**
 * host1x_syncpt_associate_device - associate device with a sync point
 * @syncpt: pointer to @host1x_syncpt
 * @dev: device to associate with
 *
 * Assign client's device to sync point.
 */
static inline void
host1x_syncpt_associate_device(struct host1x_syncpt *syncpt,
			       struct device *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&host1x_syncpts_lock, flags);
	syncpt->dev = dev;
	spin_unlock_irqrestore(&host1x_syncpts_lock, flags);
}

/**
 * host1x_syncpt_read - read sync point counter
 * @syncpt: pointer to @host1x_syncpt
 *
 * Returns sync point hardware (actual) counter value.
 */
static inline u32
host1x_syncpt_read(struct host1x_syncpt *syncpt)
{
	if (syncpt)
		return syncpt->host->syncpt_ops.read(syncpt);

	return 0;
}

/**
 * host1x_syncpt_set_interrupt - enable or disable sync point interrupt
 * @syncpt: pointer to @host1x_syncpt
 * @enabled: true enable, false to disable
 */
static inline void
host1x_syncpt_set_interrupt(struct host1x_syncpt *syncpt, bool enabled)
{
	if (syncpt)
		syncpt->host->syncpt_ops.set_interrupt(syncpt, enabled);
}

/**
 * host1x_syncpt_reset - reset sync point state
 * @syncpt: pointer to @host1x_syncpt
 * @error: errno value
 *
 * Reset @syncpt and cancel attached fences with the given @error.
 * Sync point value is reset to 0, threshold to 1 and interrupt is disabled.
 */
static inline void
host1x_syncpt_reset(struct host1x_syncpt *syncpt, int error)
{
	if (syncpt)
		syncpt->host->syncpt_ops.reset(syncpt, error);
}

/**
 * host1x_syncpt_detach_fences - detach fences from sync point
 * @syncpt: pointer to @host1x_syncpt
 */
static inline void
host1x_syncpt_detach_fences(struct host1x_syncpt *syncpt)
{
	if (syncpt)
		syncpt->host->syncpt_ops.detach_fences(syncpt);
}

/**
 * host1x_syncpt_get - bump refcount
 * @syncpt: pointer to @host1x_syncpt to bump refcount
 *
 * Returns @syncpt.
 */
static inline struct host1x_syncpt *
host1x_syncpt_get(struct host1x_syncpt *syncpt)
{
	if (syncpt)
		kref_get(&syncpt->refcount);

	return syncpt;
}

/**
 * host1x_syncpt_put - drop refcount
 * @syncpt: pointer to @host1x_syncpt to drop refcount
 *
 * Sync point released when refcounting drops to 0.
 */
static inline void
host1x_syncpt_put(struct host1x_syncpt *syncpt)
{
	if (syncpt)
		kref_put(&syncpt->refcount, syncpt->host->syncpt_ops.release);
}

/* Host1x Channel API */

/**
 * host1x_channel_request - allocate channel
 * @host: pointer to @host1x
 * @dev: device that performs the request, could be NULL
 * @num_pushbuf_words: number of words to allocate for push buffer
 *
 * Returns allocated channel on success or ERR_PTR.
 */
static inline struct host1x_channel *
host1x_channel_request(struct host1x *host, struct device *dev,
		       unsigned int num_pushbuf_words)
{
	return host->chan_ops.request(host, dev, num_pushbuf_words);
}

/**
 * host1x_channel_get - bump channels refcount
 * @chan: pointer to @host1x_channel to ref, can be NULL
 *
 * Returns @chan.
 */
static inline struct host1x_channel *
host1x_channel_get(struct host1x_channel *chan)
{
	if (chan)
		kref_get(&chan->refcount);

	return chan;
}

/**
 * host1x_channel_put - drop refcount
 * @chan: pointer to @host1x_channel to drop refcount, can be NULL
 *
 * Channel released when refcounting drops to 0.
 */
static inline void
host1x_channel_put(struct host1x_channel *chan)
{
	if (chan)
		kref_put(&chan->refcount, chan->host->chan_ops.release);
}

/**
 * host1x_channel_reset - reset channel
 * @chan: pointer to @host1x_channel to reset
 *
 * Should be invoked when job execution hangs. Resets hardware state and
 * makes channel available for further job submissions.
 */
static inline void
host1x_channel_reset(struct host1x_channel *chan)
{
	chan->host->chan_ops.reset(chan);
}

/**
 * host1x_channel_submit - submit job into channel
 * @chan: pointer to @host1x_channel
 * @job: pointer to @host1x_job to submit
 * @fence: pointer to dma_fence, can be NULL
 *
 * Pushes @job into @chan push buffer, enables @job sync point interrupt and
 * executes @job. If @fence is NULL, then a new dma_fence is allocated.
 * If @fence is *not* NULL, then the passed-in fence is used, which can be
 * used for the job's re-submitting.
 */
static inline struct dma_fence *
host1x_channel_submit(struct host1x_channel *chan,
		      struct host1x_job *job,
		      struct dma_fence *fence)
{
	return chan->host->chan_ops.submit(chan, job, fence);
}

/**
 * host1x_channel_cleanup_job - clean up channel
 * @chan: pointer to @host1x_channel
 * @job: pointer to @host1x_job to clean up
 * @fence: pointer to the jobs dma_fence
 *
 * Should be invoked when job execution hangs. Removes job from channels push
 * buffer.
 */
static inline void
host1x_channel_cleanup_job(struct host1x_channel *chan,
			   struct host1x_job *job,
			   struct dma_fence *fence)
{
	chan->host->chan_ops.cleanup_job(chan, job, fence);
}

/**
 * host1x_channel_dmaget - get channels hardware DMAGET address
 * @chan: pointer to @host1x_channel
 *
 * Reads DMAGET address from channels hardware and returns it.
 */
static inline dma_addr_t
host1x_channel_dmaget(struct host1x_channel *chan)
{
	return chan->host->chan_ops.dmaget(chan);
}

/* Host1x Buffer Object API */

/**
 * host1x_bo_alloc_standalone_data - allocate standalone memory for buffer object
 * @host: pointer to @host1x
 * @bo: pointer to @host1x_bo
 * @size: allocation size
 *
 * Allocates standalone memory for buffer object, i.e. allocated memory
 * doesn't belong to a DMA pool.
 *
 * Returns 0 on success, errno otherwise.
 */
int host1x_bo_alloc_standalone_data(struct host1x *host,
				    struct host1x_bo *bo,
				    size_t size);

/**
 * host1x_bo_free_standalone_data - release standalone memory of buffer object
 * @host: pointer to @host1x
 * @bo: pointer to @host1x_bo
 *
 * Release backing memory that was allocated by @host1x_bo_alloc_standalone_data
 */
void host1x_bo_free_standalone_data(struct host1x *host,
				    struct host1x_bo *bo);

/**
 * host1x_bo_alloc_pool_data - allocate memory for buffer object from pool
 * @host: pointer to @host1x
 * @bo: pointer to @host1x_bo
 * @size: allocation size
 *
 * Allocates memory for buffer object from DMA pool.
 *
 * Returns 0 on success, errno otherwise.
 */
static __maybe_unused inline int
host1x_bo_alloc_pool_data(struct host1x *host, struct host1x_bo *bo,
			  size_t size)
{
	struct host1x_pool_entry *e;
	unsigned long dma_attrs;
	dma_addr_t dma_api_addr;
	dma_addr_t dmaaddr;
	bool retried = false;
	void *vaddr;
	int err;

retry:
	vaddr = gen_pool_dma_alloc(host->pool, size, &dmaaddr);

	if (!vaddr) {
		if (!retried) {
			retried = true;

			err = host1x_dma_pool_grow(host, size);
			if (err)
				return err;

			goto retry;
		}

		return -ENOMEM;
	}

	spin_lock(&host->pool->lock);

	err = -EINVAL;

	/*
	 * Translate gen_pool allocation @dmaaddr into address suitable for
	 * DMA API, which could be either PHYS address or IOVA address of the
	 * implicit DMA domain.
	 */
	list_for_each_entry(e, &host->pool_chunks, list) {
		if (e->dmaaddr > dmaaddr ||
		    e->dmaaddr + (e->size - 1) < dmaaddr)
			continue;

		dma_api_addr = e->addr + (dmaaddr - e->dmaaddr);
		dma_attrs = e->dma_attrs;
		err = 0;
		break;
	}

	spin_unlock(&host->pool->lock);

	/* shouldn't happen */
	if (WARN_ON_ONCE(err)) {
		gen_pool_free(host->pool, (unsigned long) vaddr, size);
		return err;
	}

	bo->vaddr	= vaddr;
	bo->dma_attrs	= dma_attrs;
	bo->dmaaddr	= dmaaddr;
	bo->addr	= dma_api_addr;
	bo->size	= size;
	bo->from_pool	= true;

	return 0;
}

/**
 * host1x_bo_alloc_data - allocate memory for buffer object
 * @host: pointer to @host1x
 * @bo: pointer to @host1x_bo
 * @size: allocation size
 * @prefer_pool: prefer allocation from DMA pool
 *
 * Allocates memory for buffer object. Firstly tries to allocate from DMA pool
 * if @prefer_pool is true, fallbacks to standalone allocation if DMA pool
 * allocation fails or @prefer_pool is false.
 *
 * Returns 0 on success, errno otherwise.
 */
static inline int
host1x_bo_alloc_data(struct host1x *host, struct host1x_bo *bo,
		     size_t size, bool prefer_pool)
{
	int err;

	WARN_ON(bo->vaddr);

	if (prefer_pool) {
		err = host1x_bo_alloc_pool_data(host, bo, size);
		if (err)
			goto try_not_pool;
	} else {
try_not_pool:
		err = host1x_bo_alloc_standalone_data(host, bo, size);
		if (err)
			return err;
	}

	return 0;
}

/**
 * host1x_bo_free_data - free buffer object backing memory
 * @host: pointer to @host1x
 * @bo: pointer to @host1x_bo
 *
 * Free backing memory of @bo and not @bo itself.
 */
static inline void
host1x_bo_free_data(struct host1x *host, struct host1x_bo *bo)
{
	if (bo && bo->vaddr) {
		if (bo->from_pool)
			gen_pool_free(host->pool, (unsigned long) bo->vaddr,
				      bo->size);
		else
			host1x_bo_free_standalone_data(host, bo);

		bo->vaddr = NULL;
	}
}

/**
 * host1x_bo_alloc - allocate buffer object
 * @host: pointer to @host1x
 * @size: allocation size
 * @prefer_pool: prefer allocation from DMA pool
 *
 * Allocates buffer object. Firstly tries to allocate backing memory from DMA
 * pool if @prefer_pool is true, fallbacks to standalone allocation if DMA pool
 * allocation fails or @prefer_pool is false.
 *
 * Returns pointer to @host1x_bo on success, NULL otherwise.
 */
static __maybe_unused inline struct host1x_bo *
host1x_bo_alloc(struct host1x *host, size_t size, bool prefer_pool)
{
	struct host1x_bo *bo;
	int err;

	if (!size)
		return NULL;

	bo = kmalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return NULL;

	bo->vaddr = NULL;

	err = host1x_bo_alloc_data(host, bo, size, prefer_pool);
	if (err)
		goto err_free_bo;

	return bo;

err_free_bo:
	kfree(bo);

	return NULL;
}

/**
 * host1x_bo_free - free buffer object
 * @host: pointer to @host1x
 * @bo: pointer to @host1x_bo
 *
 * Free backing memory of @bo and @bo itself.
 */
static inline void
host1x_bo_free(struct host1x *host, struct host1x_bo *bo)
{
	host1x_bo_free_data(host, bo);
	kfree(bo);
}

/**
 * host1x_bo_mmap - mmap @host1x_bo
 * @host: pointer to @host1x
 * @bo: pointer to @host1x_bo
 * @vma: pointer to struct vm_area_struct
 *
 * Map memory of @host1x_bo.
 *
 * Returns 0 on success, errno otherwise.
 */
static inline int
host1x_bo_mmap(struct host1x *host, struct host1x_bo *bo,
	       struct vm_area_struct *vma)
{
	return dma_mmap_attrs(host->dev, vma, bo->vaddr, bo->addr, bo->size,
			      bo->dma_attrs);
}

/* Host1x Job API */

/**
 * host1x_init_job - initialize job structure
 * @job: pointer to @host1x_job
 * @syncpt: pointer to @host1x_syncpt
 * @context: DMA fence context
 *
 * Initializes @job structure. Shall be invoked after @job allocation.
 */
static inline void
host1x_init_job(struct host1x_job *job, struct host1x_syncpt *syncpt,
		u64 context)
{
	job->syncpt = syncpt;
	job->context = context;
	job->bo.vaddr = NULL;
	job->num_init_gathers = 0;
}

/**
 * host1x_cleanup_job - cleans up job
 * @host: pointer to @host1x
 * @job: pointer to @host1x_job
 *
 * Cleans up @job. Shall be invoked before @job destruction.
 * Shouldn't be invoked with disabled IRQ's.
 */
static inline
void host1x_cleanup_job(struct host1x *host, struct host1x_job *job)
{
	host1x_bo_free_data(host, &job->bo);
	host1x_syncpt_put(job->syncpt);

	job->syncpt = NULL;
}

/**
 * host1x_finish_job - cleans up job after completion
 * @host: pointer to @host1x
 * @job: pointer to @host1x_job
 *
 * Should be invoked after successful @job completion.
 * Could be invoked with disabled IRQ's.
 */
static inline
void host1x_finish_job(struct host1x_job *job)
{
	host1x_syncpt_put(job->syncpt);
	job->syncpt = NULL;
}

/**
 * host1x_job_add_init_gather - add init-gather to job
 * @job: pointer to @host1x_job
 * @g: pointer to @host1x_gather
 *
 * Adds "initialization" gather to the @job.
 */
static inline void
host1x_job_add_init_gather(struct host1x_job *job, struct host1x_gather *g)
{
	WARN(job->num_init_gathers == ARRAY_SIZE(job->init_gathers),
	     "please increase size of init_gathers[]");

	job->init_gathers[job->num_init_gathers++] = g;
}

/*
 * subdevice probe infrastructure
 */

struct host1x_device;

/**
 * struct host1x_driver - host1x logical device driver
 * @driver: core driver
 * @subdevs: table of OF device IDs matching subdevices for this driver
 * @list: list node for the driver
 * @probe: called when the host1x logical device is probed
 * @remove: called when the host1x logical device is removed
 * @shutdown: called when the host1x logical device is shut down
 */
struct host1x_driver {
	struct device_driver driver;

	const struct of_device_id *subdevs;
	struct list_head list;

	int (*probe)(struct host1x_device *device);
	int (*remove)(struct host1x_device *device);
	void (*shutdown)(struct host1x_device *device);
};

static inline struct host1x_driver *
to_host1x_driver(struct device_driver *driver)
{
	return container_of(driver, struct host1x_driver, driver);
}

int host1x_driver_register_full(struct host1x_driver *driver,
				struct module *owner);
void host1x_driver_unregister(struct host1x_driver *driver);

#define host1x_driver_register(driver) \
	host1x_driver_register_full(driver, THIS_MODULE)

struct host1x_device {
	struct host1x_driver *driver;
	struct list_head list;
	struct device dev;

	struct mutex subdevs_lock;
	struct list_head subdevs;
	struct list_head active;

	struct mutex clients_lock;
	struct list_head clients;

	bool registered;

	struct device_dma_parameters dma_parms;
};

/**
 * struct host1x_client - host1x client structure
 * @list: list node for the host1x client
 * @host: pointer to struct device representing the host1x controller
 * @dev: pointer to struct device backing this host1x client
 * @ops: host1x client operations
 * @class: host1x class represented by this client
 * @module: host1x module ID associated with this client
 * @syncpts: array of syncpoints requested for this client
 * @num_syncpts: number of syncpoints requested for this client
 */
struct host1x_client {
	struct list_head list;
	struct device *host;
	struct device *dev;

	const struct host1x_client_ops *ops;

	enum host1x_class class;
	enum host1x_module module;

	struct host1x_syncpt **syncpts;
	unsigned int num_syncpts;

	struct host1x_client *parent;
	unsigned int usecount;
	struct mutex lock;
};

/**
 * struct host1x_client_ops - host1x client operations
 * @init: host1x client initialization code
 * @exit: host1x client tear down code
 * @reset: host1x client HW reset code
 * @suspend: host1x client suspend code
 * @resume: host1x client resume code
 */
struct host1x_client_ops {
	int (*init)(struct host1x_client *client);
	int (*exit)(struct host1x_client *client);
	int (*reset)(struct host1x_client *client);
	int (*suspend)(struct host1x_client *client);
	int (*resume)(struct host1x_client *client);
};

static inline struct host1x_device *to_host1x_device(struct device *dev)
{
	return container_of(dev, struct host1x_device, dev);
}

int host1x_device_init(struct host1x_device *device);
int host1x_device_exit(struct host1x_device *device);

int host1x_client_register(struct host1x_client *client);
int host1x_client_unregister(struct host1x_client *client);

int host1x_client_suspend(struct host1x_client *client);
int host1x_client_resume(struct host1x_client *client);

struct tegra_mipi_device;

struct tegra_mipi_device *tegra_mipi_request(struct device *device);
void tegra_mipi_free(struct tegra_mipi_device *device);
int tegra_mipi_enable(struct tegra_mipi_device *device);
int tegra_mipi_disable(struct tegra_mipi_device *device);
int tegra_mipi_calibrate(struct tegra_mipi_device *device);

#endif
