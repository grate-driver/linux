// SPDX-License-Identifier: GPL-2.0
#define CREATE_TRACE_POINTS
#include <trace/events/mmap_lock.h>

#include <linux/mm.h>
#include <linux/atomic.h>
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
 * This is unfortunately complicated... _reg() and _unreg() may be called
 * in parallel, separately for each of our three event types. To save memory,
 * all of the event types share the same buffers. Furthermore, trace events
 * might happen in parallel with _unreg(); we need to ensure we don't free the
 * buffers before all inflights have finished. Because these events happen
 * "frequently", we also want to prevent new inflights from starting once the
 * _unreg() process begins. And, for performance reasons, we want to avoid any
 * locking in the trace event path.
 *
 * So:
 *
 * - Use a spinlock to serialize _reg() and _unreg() calls.
 * - Keep track of nested _reg() calls with a lock-protected counter.
 * - Define a flag indicating whether or not unregistration has begun (and
 *   therefore that there should be no new buffer uses going forward).
 * - Keep track of inflight buffer users with a reference count.
 */
static DEFINE_SPINLOCK(reg_lock);
static int reg_types_rc; /* Protected by reg_lock. */
static bool unreg_started; /* Doesn't need synchronization. */
/* atomic_t instead of refcount_t, as we want ordered inc without locks. */
static atomic_t inflight_rc = ATOMIC_INIT(0);

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

	/*
	 * Serialize _reg() and _unreg(). Without this, e.g. _unreg() might
	 * start cleaning up while _reg() is only partially completed.
	 */
	spin_lock_irqsave(&reg_lock, flags);

	/* If the refcount is going 0->1, proceed with allocating buffers. */
	if (reg_types_rc++)
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

	/* Reset unreg_started flag, allowing new trace events. */
	WRITE_ONCE(unreg_started, false);
	/* Add the registration +1 to the inflight refcount. */
	atomic_inc(&inflight_rc);

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

	/* Since we failed, undo the earlier increment. */
	--reg_types_rc;

	spin_unlock_irqrestore(&reg_lock, flags);
	return -ENOMEM;
}

void trace_mmap_lock_unreg(void)
{
	unsigned long flags;
	int cpu;

	spin_lock_irqsave(&reg_lock, flags);

	/* If the refcount is going 1->0, proceed with freeing buffers. */
	if (--reg_types_rc)
		goto out;

	/* This was the last registration; start preventing new events... */
	WRITE_ONCE(unreg_started, true);
	/* Remove the registration +1 from the inflight refcount. */
	atomic_dec(&inflight_rc);
	/*
	 * Wait for inflight refcount to be zero (all inflights stopped). Since
	 * we have a spinlock we can't sleep, so just spin. Because trace events
	 * are "fast", and because we stop new inflights from starting at this
	 * point with unreg_started, this should be a short spin.
	 */
	while (atomic_read(&inflight_rc))
		barrier();

	for_each_possible_cpu(cpu) {
		kfree(per_cpu(memcg_path_buf, cpu));
	}

out:
	spin_unlock_irqrestore(&reg_lock, flags);
}

static inline char *get_memcg_path_buf(void)
{
	int idx;

	/*
	 * If unregistration is happening, stop. Yes, this check is racy;
	 * that's fine. It just means _unreg() might spin waiting for an extra
	 * event or two. Use-after-free is actually prevented by the refcount.
	 */
	if (READ_ONCE(unreg_started))
		return NULL;
	/*
	 * Take a reference, unless the registration +1 has been released
	 * and there aren't already existing inflights (refcount is zero).
	 */
	if (!atomic_inc_not_zero(&inflight_rc))
		return NULL;

	idx = this_cpu_add_return(memcg_path_buf_idx, MEMCG_PATH_BUF_SIZE) -
	      MEMCG_PATH_BUF_SIZE;
	return &this_cpu_read(memcg_path_buf)[idx];
}

static inline void put_memcg_path_buf(void)
{
	this_cpu_sub(memcg_path_buf_idx, MEMCG_PATH_BUF_SIZE);
	/* We're done with this buffer; drop the reference. */
	atomic_dec(&inflight_rc);
}

/*
 * Write the given mm_struct's memcg path to a percpu buffer, and return a
 * pointer to it. If the path cannot be determined, or no buffer was available
 * (because the trace event is being unregistered), NULL is returned.
 *
 * Note: buffers are allocated per-cpu to avoid locking, so preemption must be
 * disabled by the caller before calling us, and re-enabled only after the
 * caller is done with the pointer.
 *
 * The caller must call put_memcg_path_buf() once the buffer is no longer
 * needed. This must be done while preemption is still disabled.
 */
static const char *get_mm_memcg_path(struct mm_struct *mm)
{
	char *buf = NULL;
	struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);

	if (memcg == NULL)
		goto out;
	if (unlikely(memcg->css.cgroup == NULL))
		goto out_put;

	buf = get_memcg_path_buf();
	if (buf == NULL)
		goto out_put;

	cgroup_path(memcg->css.cgroup, buf, MEMCG_PATH_BUF_SIZE);

out_put:
	css_put(&memcg->css);
out:
	return buf;
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
