/*
 * Copyright (c) 2012-2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _UAPI_TEGRA_DRM_H_
#define _UAPI_TEGRA_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

struct drm_tegra_plane_csc_blob {
	__u32 yof;
	__u32 kyrgb;
	__u32 kur;
	__u32 kvr;
	__u32 kug;
	__u32 kvg;
	__u32 kub;
	__u32 kvb;
};

#define DRM_TEGRA_GEM_CREATE_TILED			(1 << 0)
#define DRM_TEGRA_GEM_CREATE_BOTTOM_UP			(1 << 1)
#define DRM_TEGRA_GEM_CREATE_HOST1X_GATHER		(1 << 2)
#define DRM_TEGRA_GEM_CREATE_CONTIGUOUS			(1 << 3)
#define DRM_TEGRA_GEM_CREATE_SPARSE			(1 << 4)
#define DRM_TEGRA_GEM_CREATE_DONT_KMAP			(1 << 5)

/**
 * struct drm_tegra_gem_create - parameters for the GEM object creation IOCTL
 */
struct drm_tegra_gem_create {
	/**
	 * @size:
	 *
	 * The size, in bytes, of the buffer object to be created.
	 */
	__u64 size;

	/**
	 * @flags:
	 *
	 * A bitmask of flags that influence the creation of GEM objects:
	 *
	 * DRM_TEGRA_GEM_CREATE_TILED
	 *   Use the 16x16 tiling format for this buffer.
	 *
	 * DRM_TEGRA_GEM_CREATE_BOTTOM_UP
	 *   The buffer has a bottom-up layout.
	 *
	 * DRM_TEGRA_GEM_CREATE_HOST1X_GATHER
	 *   The buffer is host1x gather, it can't be used as framebuffer.
	 *
	 * DRM_TEGRA_GEM_CREATE_CONTIGUOUS
	 *   The buffer is to be backed by physically contiguous memory.
	 *
	 *   On Tegra20 contiguous allocation is the default, unless "sparse"
	 *   flag is set.
	 *
	 * DRM_TEGRA_GEM_CREATE_SPARSE
	 *   The buffer is to be backed by physically sparse memory.
	 *
	 *   On Tegra30+ sparse allocation is the default, unless "contiguous"
	 *   flag is set or IOMMU is disabled. The "contiguous" flag takes
	 *   precedence when both flags are set.
	 *
	 * DRM_TEGRA_GEM_CREATE_DONT_KMAP
	 *   Hint to the driver that there is no need to map GEM into kernel
	 *   space.
	 */
	__u32 flags;

	/**
	 * @handle:
	 *
	 * The handle of the created GEM object. Set by the kernel upon
	 * successful completion of the IOCTL.
	 */
	__u32 handle;
};

/**
 * struct drm_tegra_gem_mmap - parameters for the GEM mmap IOCTL
 */
struct drm_tegra_gem_mmap {
	/**
	 * @handle:
	 *
	 * Handle of the GEM object to obtain an mmap offset for.
	 */
	__u32 handle;

	/**
	 * @pad:
	 *
	 * Structure padding that may be used in the future. Must be 0.
	 */
	__u32 pad;

	/**
	 * @offset:
	 *
	 * The mmap offset for the given GEM object. Set by the kernel upon
	 * successful completion of the IOCTL.
	 */
	__u64 offset;
};

/**
 * struct drm_tegra_syncpt_read - parameters for the read syncpoint IOCTL
 */
struct drm_tegra_syncpt_read {
	/**
	 * @id:
	 *
	 * ID of the syncpoint to read the current value from.
	 */
	__u32 id;

	/**
	 * @value:
	 *
	 * The current syncpoint value. Set by the kernel upon successful
	 * completion of the IOCTL.
	 */
	__u32 value;
};

/**
 * struct drm_tegra_syncpt_incr - parameters for the increment syncpoint IOCTL
 */
struct drm_tegra_syncpt_incr {
	/**
	 * @id:
	 *
	 * ID of the syncpoint to increment.
	 */
	__u32 id;

	/**
	 * @pad:
	 *
	 * Structure padding that may be used in the future. Must be 0.
	 */
	__u32 pad;
};

/**
 * struct drm_tegra_syncpt_wait - parameters for the wait syncpoint IOCTL
 */
struct drm_tegra_syncpt_wait {
	/**
	 * @id:
	 *
	 * ID of the syncpoint to wait on.
	 */
	__u32 id;

	/**
	 * @thresh:
	 *
	 * Threshold value for which to wait.
	 */
	__u32 thresh;

	/**
	 * @timeout:
	 *
	 * Timeout, in milliseconds, to wait.
	 */
	__u32 timeout;

	/**
	 * @value:
	 *
	 * The new syncpoint value after the wait. Set by the kernel upon
	 * successful completion of the IOCTL.
	 */
	__u32 value;
};

#define DRM_TEGRA_NO_TIMEOUT	(0xffffffff)

/**
 * struct drm_tegra_open_channel - parameters for the open channel IOCTL
 */
struct drm_tegra_open_channel {
	/**
	 * @client:
	 *
	 * The client ID for this channel.
	 */
	__u32 client;

	/**
	 * @pad:
	 *
	 * Structure padding that may be used in the future. Must be 0.
	 */
	__u32 pad;

	/**
	 * @context:
	 *
	 * The application context of this channel. Set by the kernel upon
	 * successful completion of the IOCTL. This context needs to be passed
	 * to the DRM_TEGRA_CHANNEL_CLOSE or the DRM_TEGRA_SUBMIT IOCTLs.
	 */
	__u64 context;
};

/**
 * struct drm_tegra_close_channel - parameters for the close channel IOCTL
 */
struct drm_tegra_close_channel {
	/**
	 * @context:
	 *
	 * The application context of this channel. This is obtained from the
	 * DRM_TEGRA_OPEN_CHANNEL IOCTL.
	 */
	__u64 context;
};

/**
 * struct drm_tegra_get_syncpt - parameters for the get syncpoint IOCTL
 */
struct drm_tegra_get_syncpt {
	/**
	 * @context:
	 *
	 * The application context identifying the channel for which to obtain
	 * the syncpoint ID.
	 */
	__u64 context;

	/**
	 * @index:
	 *
	 * Index of the client syncpoint for which to obtain the ID.
	 */
	__u32 index;

	/**
	 * @id:
	 *
	 * The ID of the given syncpoint. Set by the kernel upon successful
	 * completion of the IOCTL.
	 */
	__u32 id;
};

/**
 * struct drm_tegra_get_syncpt_base - parameters for the get wait base IOCTL
 */
struct drm_tegra_get_syncpt_base {
	/**
	 * @context:
	 *
	 * The application context identifying for which channel to obtain the
	 * wait base.
	 */
	__u64 context;

	/**
	 * @syncpt:
	 *
	 * ID of the syncpoint for which to obtain the wait base.
	 */
	__u32 syncpt;

	/**
	 * @id:
	 *
	 * The ID of the wait base corresponding to the client syncpoint. Set
	 * by the kernel upon successful completion of the IOCTL.
	 */
	__u32 id;
};

/**
 * struct drm_tegra_syncpt - syncpoint increment operation
 */
struct drm_tegra_syncpt {
	/**
	 * @id:
	 *
	 * ID of the syncpoint to operate on.
	 */
	__u32 id;

	/**
	 * @incrs:
	 *
	 * Number of increments to perform for the syncpoint.
	 */
	__u32 incrs;
};

/**
 * struct drm_tegra_cmdbuf - structure describing a command buffer
 */
struct drm_tegra_cmdbuf {
	/**
	 * @handle:
	 *
	 * Handle to a GEM object containing the command buffer.
	 */
	__u32 handle;

	/**
	 * @offset:
	 *
	 * Offset, in bytes, into the GEM object identified by @handle at
	 * which the command buffer starts.
	 */
	__u32 offset;

	/**
	 * @words:
	 *
	 * Number of 32-bit words in this command buffer.
	 */
	__u32 words;

	/**
	 * @pad:
	 *
	 * Structure padding that may be used in the future. Must be 0.
	 */
	__u32 pad;
};

/**
 * struct drm_tegra_reloc - GEM object relocation structure
 */
struct drm_tegra_reloc {
	struct {
		/**
		 * @cmdbuf.handle:
		 *
		 * Handle to the GEM object containing the command buffer for
		 * which to perform this GEM object relocation.
		 */
		__u32 handle;

		/**
		 * @cmdbuf.offset:
		 *
		 * Offset, in bytes, into the command buffer at which to
		 * insert the relocated address.
		 */
		__u32 offset;
	} cmdbuf;
	struct {
		/**
		 * @target.handle:
		 *
		 * Handle to the GEM object to be relocated.
		 */
		__u32 handle;

		/**
		 * @target.offset:
		 *
		 * Offset, in bytes, into the target GEM object at which the
		 * relocated data starts.
		 */
		__u32 offset;
	} target;

	/**
	 * @shift:
	 *
	 * The number of bits by which to shift relocated addresses.
	 */
	__u32 shift;

	/**
	 * @pad:
	 *
	 * Structure padding that may be used in the future. Must be 0.
	 */
	__u32 pad;
};

/**
 * struct drm_tegra_waitchk - wait check structure
 */
struct drm_tegra_waitchk {
	/**
	 * @handle:
	 *
	 * Handle to the GEM object containing a command stream on which to
	 * perform the wait check.
	 */
	__u32 handle;

	/**
	 * @offset:
	 *
	 * Offset, in bytes, of the location in the command stream to perform
	 * the wait check on.
	 */
	__u32 offset;

	/**
	 * @syncpt:
	 *
	 * ID of the syncpoint to wait check.
	 */
	__u32 syncpt;

	/**
	 * @thresh:
	 *
	 * Threshold value for which to check.
	 */
	__u32 thresh;
};

/**
 * struct drm_tegra_submit - job submission structure
 */
struct drm_tegra_submit {
	/**
	 * @context:
	 *
	 * The application context identifying the channel to use for the
	 * execution of this job.
	 */
	__u64 context;

	/**
	 * @num_syncpts:
	 *
	 * The number of syncpoints operated on by this job. This defines the
	 * length of the array pointed to by @syncpts.
	 */
	__u32 num_syncpts;

	/**
	 * @num_cmdbufs:
	 *
	 * The number of command buffers to execute as part of this job. This
	 * defines the length of the array pointed to by @cmdbufs.
	 */
	__u32 num_cmdbufs;

	/**
	 * @num_relocs:
	 *
	 * The number of relocations to perform before executing this job.
	 * This defines the length of the array pointed to by @relocs.
	 */
	__u32 num_relocs;

	/**
	 * @num_waitchks:
	 *
	 * The number of wait checks to perform as part of this job. This
	 * defines the length of the array pointed to by @waitchks.
	 */
	__u32 num_waitchks;

	/**
	 * @waitchk_mask:
	 *
	 * Bitmask of valid wait checks.
	 */
	__u32 waitchk_mask;

	/**
	 * @timeout:
	 *
	 * Timeout, in milliseconds, before this job is cancelled.
	 */
	__u32 timeout;

	/**
	 * @syncpts:
	 *
	 * A pointer to an array of &struct drm_tegra_syncpt structures that
	 * specify the syncpoint operations performed as part of this job.
	 * The number of elements in the array must be equal to the value
	 * given by @num_syncpts.
	 */
	__u64 syncpts;

	/**
	 * @cmdbufs:
	 *
	 * A pointer to an array of &struct drm_tegra_cmdbuf structures that
	 * define the command buffers to execute as part of this job. The
	 * number of elements in the array must be equal to the value given
	 * by @num_syncpts.
	 */
	__u64 cmdbufs;

	/**
	 * @relocs:
	 *
	 * A pointer to an array of &struct drm_tegra_reloc structures that
	 * specify the relocations that need to be performed before executing
	 * this job. The number of elements in the array must be equal to the
	 * value given by @num_relocs.
	 */
	__u64 relocs;

	/**
	 * @waitchks:
	 *
	 * A pointer to an array of &struct drm_tegra_waitchk structures that
	 * specify the wait checks to be performed while executing this job.
	 * The number of elements in the array must be equal to the value
	 * given by @num_waitchks.
	 */
	__u64 waitchks;

	/**
	 * @fence:
	 *
	 * The threshold of the syncpoint associated with this job after it
	 * has been completed. Set by the kernel upon successful completion of
	 * the IOCTL. This can be used with the DRM_TEGRA_SYNCPT_WAIT IOCTL to
	 * wait for this job to be finished.
	 */
	__u32 fence;

	/**
	 * @reserved:
	 *
	 * This field is reserved for future use. Must be 0.
	 */
	__u32 reserved[5];
};

#define DRM_TEGRA_GEM_TILING_MODE_PITCH 0
#define DRM_TEGRA_GEM_TILING_MODE_TILED 1
#define DRM_TEGRA_GEM_TILING_MODE_BLOCK 2

/**
 * struct drm_tegra_gem_set_tiling - parameters for the set tiling IOCTL
 */
struct drm_tegra_gem_set_tiling {
	/**
	 * @handle:
	 *
	 * Handle to the GEM object for which to set the tiling parameters.
	 */
	__u32 handle;

	/**
	 * @mode:
	 *
	 * The tiling mode to set. Must be one of:
	 *
	 * DRM_TEGRA_GEM_TILING_MODE_PITCH
	 *   pitch linear format
	 *
	 * DRM_TEGRA_GEM_TILING_MODE_TILED
	 *   16x16 tiling format
	 *
	 * DRM_TEGRA_GEM_TILING_MODE_BLOCK
	 *   16Bx2 tiling format
	 */
	__u32 mode;

	/**
	 * @value:
	 *
	 * The value to set for the tiling mode parameter.
	 */
	__u32 value;

	/**
	 * @pad:
	 *
	 * Structure padding that may be used in the future. Must be 0.
	 */
	__u32 pad;
};

/**
 * struct drm_tegra_gem_get_tiling - parameters for the get tiling IOCTL
 */
struct drm_tegra_gem_get_tiling {
	/**
	 * @handle:
	 *
	 * Handle to the GEM object for which to query the tiling parameters.
	 */
	__u32 handle;

	/**
	 * @mode:
	 *
	 * The tiling mode currently associated with the GEM object. Set by
	 * the kernel upon successful completion of the IOCTL.
	 */
	__u32 mode;

	/**
	 * @value:
	 *
	 * The tiling mode parameter currently associated with the GEM object.
	 * Set by the kernel upon successful completion of the IOCTL.
	 */
	__u32 value;

	/**
	 * @pad:
	 *
	 * Structure padding that may be used in the future. Must be 0.
	 */
	__u32 pad;
};

#define DRM_TEGRA_GEM_BOTTOM_UP		(1 << 0)
#define DRM_TEGRA_GEM_SPARSE		(1 << 1)
#define DRM_TEGRA_GEM_FLAGS		(DRM_TEGRA_GEM_BOTTOM_UP)

/**
 * struct drm_tegra_gem_set_flags - parameters for the set flags IOCTL
 */
struct drm_tegra_gem_set_flags {
	/**
	 * @handle:
	 *
	 * Handle to the GEM object for which to set the flags.
	 */
	__u32 handle;

	/**
	 * @flags:
	 *
	 * The flags to set for the GEM object.
	 */
	__u32 flags;
};

/**
 * struct drm_tegra_gem_get_flags - parameters for the get flags IOCTL
 */
struct drm_tegra_gem_get_flags {
	/**
	 * @handle:
	 *
	 * Handle to the GEM object for which to query the flags.
	 */
	__u32 handle;

	/**
	 * @flags:
	 *
	 * The flags currently associated with the GEM object. Set by the
	 * kernel upon successful completion of the IOCTL.
	 */
	__u32 flags;
};

#define DRM_TEGRA_CPU_PREP_WRITE		(1 << 0)
#define DRM_TEGRA_CPU_PREP_WRITE_SYNC		(1 << 1)
#define DRM_TEGRA_CPU_PREP_READ_SYNC		(1 << 2)
#define DRM_TEGRA_CPU_PREP_FLAGS		(DRM_TEGRA_CPU_PREP_WRITE |\
						 DRM_TEGRA_CPU_PREP_WRITE_SYNC |\
						 DRM_TEGRA_CPU_PREP_READ_SYNC)

/**
 * struct drm_tegra_gem_cpu_prep - prepare to access GEM's memory
 */
struct drm_tegra_gem_cpu_prep {
	/**
	 * @handle:
	 *
	 * Handle of the GEM object to prepare.
	 */
	__u32 handle;

	/**
	 * @flags:
	 *
	 * A bitmask of flags that specifies how CPU access shall be prepared.
	 *
	 * DRM_TEGRA_CPU_PREP_WRITE
	 *   Wait for all in-flight writes to the GEM's memory to be completed.
	 *
	 * DRM_TEGRA_CPU_PREP_WRITE_SYNC
	 *   Flush out GEM's data out of CPU caches to DRAM.
	 *
	 * DRM_TEGRA_CPU_PREP_READ_SYNC
	 *   Invalidate GEM's data in CPU caches.
	 */
	__u32 flags;

	/**
	 * @timeout:
	 *
	 * Timeout value in microseconds after which waiting operation
	 * is canceled.
	 */
	__u32 timeout;
};

/**
 * enum drm_tegra_client_pipe_id - possible pipe ids
 *
 * Enumeration of hardware pipes.
 */
enum drm_tegra_client_pipe_id {
	/**
	 * @DRM_TEGRA_PIPE_ID_2D:
	 *
	 * Pipe to GR2D hardware unit.
	 */
	DRM_TEGRA_PIPE_ID_2D,
	/**
	 * @DRM_TEGRA_PIPE_ID_3D:
	 *
	 * Pipe to GR3D hardware unit.
	 */
	DRM_TEGRA_PIPE_ID_3D,
	/**
	 * @DRM_TEGRA_PIPE_ID_VIC:
	 *
	 * Pipe to VIC hardware unit.
	 */
	DRM_TEGRA_PIPE_ID_VIC,
};

/**
 * enum drm_tegra_cmdstream_class - command stream classes
 *
 * Enumeration host1x classes to be used by @drm_tegra_submit_v2 within
 * @cmdstream_ptr.
 */
enum drm_tegra_cmdstream_class {
	/**
	 * @DRM_TEGRA_CMDSTREAM_CLASS_HOST1X
	 *
	 * Host1x class ID.
	 */
	DRM_TEGRA_CMDSTREAM_CLASS_HOST1X = 0x01,
	/**
	 * @DRM_TEGRA_CMDSTREAM_CLASS_GR2D_G2
	 *
	 * GR2D G2 (generic) class ID.
	 */
	DRM_TEGRA_CMDSTREAM_CLASS_GR2D_G2 = 0x51,
	/**
	 * @DRM_TEGRA_CMDSTREAM_CLASS_GR2D_SB
	 *
	 * GR2D SB (surface-blitter) class ID.
	 */
	DRM_TEGRA_CMDSTREAM_CLASS_GR2D_SB = 0x52,
	/**
	 * @DRM_TEGRA_CMDSTREAM_CLASS_GR3D
	 *
	 * GR3D class ID.
	 */
	DRM_TEGRA_CMDSTREAM_CLASS_GR3D = 0x60,
	/**
	 * @DRM_TEGRA_CMDSTREAM_CLASS_VIC
	 *
	 * VIC class ID.
	 */
	DRM_TEGRA_CMDSTREAM_CLASS_VIC = 0x5D,
};

/**
 * struct drm_tegra_cmdstream_reloc - buffer object relocation descriptor
 *
 * Used by @drm_tegra_submit_v2 within @cmdstream_ptr in place of memory
 * addresses.
 */
struct drm_tegra_cmdstream_reloc {
	union {
		struct {
			/**
			 * @bo_index:
			 *
			 * Buffer object index within @bo_table_ptr.
			 */
			__u32 bo_index : 6;

			/**
			 * @bo_offset:
			 *
			 * Offset in bytes that is added to buffer object's
			 * memory address for generic relocations.
			 *
			 * Offset in words that is added to buffer object's
			 * memory address for gather relocations.
			 */
			__u32 bo_offset : 26;
		};

		/**
		 * @u_data:
		 *
		 * U32 word that contains @bo_index and @bo_offset to be used
		 * within @cmdstream_ptr.
		 */
		__u32 u_data;
	};
};

/**
 * struct drm_tegra_cmdstream_wait_syncpt - sync point wait descriptor
 *
 * Used by @drm_tegra_submit_v2 within @cmdstream_ptr in place of WAIT_SYNCPT
 * register (method) data.
 */
struct drm_tegra_cmdstream_wait_syncpt {
	union {
		struct {
			/**
			 * @threshold:
			 *
			 * Threshold value to wait for. Could be 0, which
			 * is a special Tegra DRM UAPI case that means to
			 * wait for the latest sync point increment.
			 */
			__u32 threshold : 24;
		};

		/**
		 * @u_data:
		 *
		 * U32 word that contains @threshold to be used within
		 * @cmdstream_ptr.
		 */
		__u32 u_data;
	};
};

/**
 * struct drm_tegra_cmdstream_extend_op - extended host1x command descriptor
 *
 * Used by @drm_tegra_submit_v2 within @cmdstream_ptr in place of "EXTEND"
 * opcode.
 */
struct drm_tegra_cmdstream_extend_op {
	union {
		struct {
			/**
			 * @value:
			 *
			 * EXTEND value. For ACQUIRE_MLOCK / RELEASE_MLOCK
			 * subops it is one of @drm_tegra_client_pipe_id.
			 */
			__u32 value : 24;

			/**
			 * @subop:
			 *
			 * EXTEND subop.
			 */
			__u32 subop : 4;

			/**
			 * @opcode:
			 *
			 * Host1x opcode.
			 */
			__u32 opcode : 4;
		};

		/**
		 * @u_data:
		 *
		 * U32 word that contains @drm_tegra_cmdstream_extend_op data.
		 */
		__u32 u_data;
	};
};

#define DRM_TEGRA_BO_TABLE_MAX_ENTRIES_NUM	64

#define DRM_TEGRA_BO_TABLE_WRITE		(1 << 0)
#define DRM_TEGRA_BO_TABLE_EXPLICIT_FENCE	(1 << 1)

/**
 * struct drm_tegra_bo_table_entry - buffer object table entry
 *
 * Contain job's buffer object description, it is referenced by
 * @drm_tegra_cmdstream_reloc. Must not contain more entries than
 * @DRM_TEGRA_BO_TABLE_MAX_ENTRIES_NUM.
 */
struct drm_tegra_bo_table_entry {
	/**
	 * @handle:
	 *
	 * Buffer object handle ID.
	 */
	__u32 handle;

	/**
	 * @flags:
	 *
	 * Bitmask of table entry flags.
	 *
	 * DRM_TEGRA_BO_TABLE_WRITE
	 *   Job writes data to BO.
	 *
	 * DRM_TEGRA_BO_TABLE_EXPLICIT_FENCE
	 *   Job execution won't be stalled by awaiting for the implicit BO
	 *   fences.
	 */
	__u32 flags;
};

/**
 * struct drm_tegra_submit_v2 - job submission version 2
 *
 * Each job consists of channel DMA commands stream data, consult Technical
 * Reference Manual for the opcodes and encoding.
 *
 * The sensitive data-writes, like memory addresses and sync point ID's, are
 * specified using Tegra DRM UAPI encoding. Their values are substituted
 * with actual HW values during of the job-patching process.
 */
struct drm_tegra_submit_v2 {
	/**
	 * @pipes:
	 *
	 * The bitmask of @drm_tegra_client_pipe_id that is used to annotate
	 * what HW units are utilized by the job.
	 */
	__u64 pipes;

	/**
	 * @cmdstream_ptr:
	 *
	 * Userspace memory address that points to the beginning of buffer
	 * that contains commands stream data.
	 */
	__u64 cmdstream_ptr;

	/**
	 * @bo_table_ptr:
	 *
	 * Userspace memory address that points to the beginning of buffer
	 * that contains array of @drm_tegra_bo_table_entry. Entries must
	 * be unique.
	 */
	__u64 bo_table_ptr;

	/**
	 * @num_cmdstream_words:
	 *
	 * Number of u32 words contained in @cmdstream_ptr.
	 */
	__u32 num_cmdstream_words;

	/**
	 * @num_bos:
	 *
	 * Number of entries contained in @bo_table_ptr.
	 */
	__u32 num_bos;

	/**
	 * @flags:
	 *
	 * A bitmask of the following flags:
	 */
	__u32 flags;

	/**
	 * @in_fence:
	 *
	 * Handle ID of sync object containing dma_fence that shall be
	 * signalled before job could be executed. Could be 0, which
	 * tells to skip the in-fence.
	 */
	__u32 in_fence;

	/**
	 * @out_fence:
	 *
	 * Handle ID of sync object to be used for attaching of job's
	 * completion dma_fence. Could be 0, which tells to skip attaching
	 * of the out-fence.
	 */
	__u32 out_fence;

	/**
	 * @uapi_ver:
	 *
	 * UAPI version of job's data that is copied from userspace:
	 * @cmdstream_ptr, @drm_tegra_bo_table_entry.
	 */
	__u32 uapi_ver;
};

/**
 * enum drm_tegra_version - enumeration of SoC versions
 */
enum drm_tegra_soc_version {
	DRM_TEGRA_SOC_T20,
	DRM_TEGRA_SOC_T30,
	DRM_TEGRA_SOC_T114,
	DRM_TEGRA_SOC_T124,
	DRM_TEGRA_SOC_T132,
	DRM_TEGRA_SOC_T148,
	DRM_TEGRA_SOC_T210,
	DRM_TEGRA_SOC_T186,
	DRM_TEGRA_SOC_T194,
};

/**
 * struct drm_tegra_version - hardware and UAPI versions
 */
struct drm_tegra_version {
	/**
	 * @soc_ver:
	 *
	 * Value containing @drm_tegra_soc_version
	 */
	__u32 soc_ver;

	/**
	 * @uapi_ver:
	 *
	 * Value containing maximum version of supported UAPI.
	 */
	__u32 uapi_ver;
};

#define DRM_TEGRA_GEM_CREATE		0x00
#define DRM_TEGRA_GEM_MMAP		0x01
#define DRM_TEGRA_SYNCPT_READ		0x02
#define DRM_TEGRA_SYNCPT_INCR		0x03
#define DRM_TEGRA_SYNCPT_WAIT		0x04
#define DRM_TEGRA_OPEN_CHANNEL		0x05
#define DRM_TEGRA_CLOSE_CHANNEL		0x06
#define DRM_TEGRA_GET_SYNCPT		0x07
#define DRM_TEGRA_SUBMIT		0x08
#define DRM_TEGRA_GET_SYNCPT_BASE	0x09
#define DRM_TEGRA_GEM_SET_TILING	0x0a
#define DRM_TEGRA_GEM_GET_TILING	0x0b
#define DRM_TEGRA_GEM_SET_FLAGS		0x0c
#define DRM_TEGRA_GEM_GET_FLAGS		0x0d
#define DRM_TEGRA_GEM_CPU_PREP		0x0e
#define DRM_TEGRA_SUBMIT_V2		0x0f
#define DRM_TEGRA_VERSION		0x10

#define DRM_IOCTL_TEGRA_GEM_CREATE DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_GEM_CREATE, struct drm_tegra_gem_create)
#define DRM_IOCTL_TEGRA_GEM_MMAP DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_GEM_MMAP, struct drm_tegra_gem_mmap)
#define DRM_IOCTL_TEGRA_SYNCPT_READ DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_SYNCPT_READ, struct drm_tegra_syncpt_read)
#define DRM_IOCTL_TEGRA_SYNCPT_INCR DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_SYNCPT_INCR, struct drm_tegra_syncpt_incr)
#define DRM_IOCTL_TEGRA_SYNCPT_WAIT DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_SYNCPT_WAIT, struct drm_tegra_syncpt_wait)
#define DRM_IOCTL_TEGRA_OPEN_CHANNEL DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_OPEN_CHANNEL, struct drm_tegra_open_channel)
#define DRM_IOCTL_TEGRA_CLOSE_CHANNEL DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_CLOSE_CHANNEL, struct drm_tegra_close_channel)
#define DRM_IOCTL_TEGRA_GET_SYNCPT DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_GET_SYNCPT, struct drm_tegra_get_syncpt)
#define DRM_IOCTL_TEGRA_SUBMIT DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_SUBMIT, struct drm_tegra_submit)
#define DRM_IOCTL_TEGRA_GET_SYNCPT_BASE DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_GET_SYNCPT_BASE, struct drm_tegra_get_syncpt_base)
#define DRM_IOCTL_TEGRA_GEM_SET_TILING DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_GEM_SET_TILING, struct drm_tegra_gem_set_tiling)
#define DRM_IOCTL_TEGRA_GEM_GET_TILING DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_GEM_GET_TILING, struct drm_tegra_gem_get_tiling)
#define DRM_IOCTL_TEGRA_GEM_SET_FLAGS DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_GEM_SET_FLAGS, struct drm_tegra_gem_set_flags)
#define DRM_IOCTL_TEGRA_GEM_GET_FLAGS DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_GEM_GET_FLAGS, struct drm_tegra_gem_get_flags)
#define DRM_IOCTL_TEGRA_GEM_CPU_PREP DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_GEM_CPU_PREP, struct drm_tegra_gem_cpu_prep)
#define DRM_IOCTL_TEGRA_SUBMIT_V2 DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_SUBMIT_V2, struct drm_tegra_submit_v2)
#define DRM_IOCTL_TEGRA_VERSION DRM_IOWR(DRM_COMMAND_BASE + DRM_TEGRA_VERSION, struct drm_tegra_version)

#if defined(__cplusplus)
}
#endif

#endif
