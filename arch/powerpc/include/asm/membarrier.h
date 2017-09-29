#ifndef _ASM_POWERPC_MEMBARRIER_H
#define _ASM_POWERPC_MEMBARRIER_H

static inline void membarrier_arch_switch_mm(struct mm_struct *prev,
		struct mm_struct *next, struct task_struct *tsk)
{
	/*
	 * Only need the full barrier when switching between processes.
	 * Barrier when switching from kernel to userspace is not
	 * required here, given that it is implied by mmdrop(). Barrier
	 * when switching from userspace to kernel is not needed after
	 * store to rq->curr.
	 */
	if (likely(!test_ti_thread_flag(task_thread_info(tsk),
			TIF_MEMBARRIER_PRIVATE_EXPEDITED) || !prev))
		return;

	/*
	 * The membarrier system call requires a full memory barrier
	 * after storing to rq->curr, before going back to user-space.
	 */
	smp_mb();
}
static inline void membarrier_arch_fork(struct task_struct *t,
		unsigned long clone_flags)
{
	/*
	 * Coherence of TIF_MEMBARRIER_PRIVATE_EXPEDITED against thread
	 * fork is protected by siglock. membarrier_arch_fork is called
	 * with siglock held.
	 */
	if (test_thread_flag(TIF_MEMBARRIER_PRIVATE_EXPEDITED))
		set_ti_thread_flag(task_thread_info(t),
				TIF_MEMBARRIER_PRIVATE_EXPEDITED);
}
static inline void membarrier_arch_execve(struct task_struct *t)
{
	clear_ti_thread_flag(task_thread_info(t),
			TIF_MEMBARRIER_PRIVATE_EXPEDITED);
}
void membarrier_arch_register_private_expedited(struct task_struct *t);

#endif /* _ASM_POWERPC_MEMBARRIER_H */
