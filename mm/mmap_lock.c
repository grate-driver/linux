// SPDX-License-Identifier: GPL-2.0
#define CREATE_TRACE_POINTS
#include <trace/events/mmap_lock.h>

#include <linux/mm.h>
#include <linux/cgroup.h>
#include <linux/memcontrol.h>
#include <linux/mmap_lock.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/trace_events.h>

EXPORT_TRACEPOINT_SYMBOL(mmap_lock_start_locking);
EXPORT_TRACEPOINT_SYMBOL(mmap_lock_acquire_returned);
EXPORT_TRACEPOINT_SYMBOL(mmap_lock_released);

#ifdef CONFIG_MEMCG

/*
 * Our various events all share the same buffer (because we don't want or need
 * to allocate a set of buffers *per event type*), so we need to protect against
 * concurrent _reg() and _unreg() calls, and count how many _reg() calls have
 * been made.
 */
static DEFINE_SPINLOCK(reg_lock);
static int reg_refcount;

/*
 * Size of the buffer for memcg path names. Ignoring stack trace support,
 * trace_events_hist.c uses MAX_FILTER_STR_VAL for this, so we also use it.
 */
#define MEMCG_PATH_BUF_SIZE MAX_FILTER_STR_VAL

/*
 * How many contexts our trace events might be called in: normal, softirq, irq,
 * and NMI.
 */
#define CONTEXT_COUNT 4

DEFINE_PER_CPU(char *, memcg_path_buf);
DEFINE_PER_CPU(int, memcg_path_buf_idx);

int trace_mmap_lock_reg(void)
{
	unsigned long flags;
	int cpu;

	spin_lock_irqsave(&reg_lock, flags);

	if (reg_refcount++)
		goto out;

	for_each_possible_cpu(cpu) {
		per_cpu(memcg_path_buf, cpu) = NULL;
	}
	for_each_possible_cpu(cpu) {
		per_cpu(memcg_path_buf, cpu) = kmalloc(
			MEMCG_PATH_BUF_SIZE * CONTEXT_COUNT, GFP_NOWAIT);
		if (per_cpu(memcg_path_buf, cpu) == NULL)
			goto out_fail;
		per_cpu(memcg_path_buf_idx, cpu) = 0;
	}

out:
	spin_unlock_irqrestore(&reg_lock, flags);
	return 0;

out_fail:
	for_each_possible_cpu(cpu) {
		if (per_cpu(memcg_path_buf, cpu) != NULL)
			kfree(per_cpu(memcg_path_buf, cpu));
		else
			break;
	}

	--reg_refcount;

	spin_unlock_irqrestore(&reg_lock, flags);
	return -ENOMEM;
}

void trace_mmap_lock_unreg(void)
{
	unsigned long flags;
	int cpu;

	spin_lock_irqsave(&reg_lock, flags);

	if (--reg_refcount)
		goto out;

	for_each_possible_cpu(cpu) {
		kfree(per_cpu(memcg_path_buf, cpu));
	}

out:
	spin_unlock_irqrestore(&reg_lock, flags);
}

static inline char *get_memcg_path_buf(void)
{
	int idx;

	idx = this_cpu_add_return(memcg_path_buf_idx, MEMCG_PATH_BUF_SIZE) -
	      MEMCG_PATH_BUF_SIZE;
	return &this_cpu_read(memcg_path_buf)[idx];
}

static inline void put_memcg_path_buf(void)
{
	this_cpu_sub(memcg_path_buf_idx, MEMCG_PATH_BUF_SIZE);
}

/*
 * Write the given mm_struct's memcg path to a percpu buffer, and return a
 * pointer to it. If the path cannot be determined, NULL is returned.
 *
 * Note: buffers are allocated per-cpu to avoid locking, so preemption must be
 * disabled by the caller before calling us, and re-enabled only after the
 * caller is done with the pointer.
 */
static const char *get_mm_memcg_path(struct mm_struct *mm)
{
	struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);

	if (memcg != NULL && likely(memcg->css.cgroup != NULL)) {
		char *buf = get_memcg_path_buf();

		cgroup_path(memcg->css.cgroup, buf, MEMCG_PATH_BUF_SIZE);
		return buf;
	}
	return NULL;
}

#define TRACE_MMAP_LOCK_EVENT(type, mm, ...)                                   \
	do {                                                                   \
		const char *memcg_path;                                        \
		preempt_disable();                                             \
		memcg_path = get_mm_memcg_path(mm);                            \
		trace_mmap_lock_##type(mm,                                     \
				       memcg_path != NULL ? memcg_path : "",   \
				       ##__VA_ARGS__);                         \
		if (likely(memcg_path != NULL))                                \
			put_memcg_path_buf();                                  \
		preempt_enable();                                              \
	} while (0)

#else /* !CONFIG_MEMCG */

int trace_mmap_lock_reg(void)
{
	return 0;
}

void trace_mmap_lock_unreg(void)
{
}

#define TRACE_MMAP_LOCK_EVENT(type, mm, ...)                                   \
	trace_mmap_lock_##type(mm, "", ##__VA_ARGS__)

#endif /* CONFIG_MEMCG */

/*
 * Trace calls must be in a separate file, as otherwise there's a circular
 * dependency between linux/mmap_lock.h and trace/events/mmap_lock.h.
 */

void __mmap_lock_do_trace_start_locking(struct mm_struct *mm, bool write)
{
	TRACE_MMAP_LOCK_EVENT(start_locking, mm, write);
}
EXPORT_SYMBOL(__mmap_lock_do_trace_start_locking);

void __mmap_lock_do_trace_acquire_returned(struct mm_struct *mm, bool write,
					   bool success)
{
	TRACE_MMAP_LOCK_EVENT(acquire_returned, mm, write, success);
}
EXPORT_SYMBOL(__mmap_lock_do_trace_acquire_returned);

void __mmap_lock_do_trace_released(struct mm_struct *mm, bool write)
{
	TRACE_MMAP_LOCK_EVENT(released, mm, write);
}
EXPORT_SYMBOL(__mmap_lock_do_trace_released);
