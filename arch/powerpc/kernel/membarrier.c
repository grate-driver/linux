/*
 * Copyright (C) 2010-2017 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * membarrier system call - PowerPC architecture code
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/thread_info.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>

void membarrier_arch_register_private_expedited(struct task_struct *p)
{
	struct task_struct *t;

	if (get_nr_threads(p) == 1) {
		set_thread_flag(TIF_MEMBARRIER_PRIVATE_EXPEDITED);
		return;
	}
	/*
	 * Coherence of TIF_MEMBARRIER_PRIVATE_EXPEDITED against thread
	 * fork is protected by siglock.
	 */
	spin_lock(&p->sighand->siglock);
	for_each_thread(p, t)
		set_ti_thread_flag(task_thread_info(t),
				TIF_MEMBARRIER_PRIVATE_EXPEDITED);
	spin_unlock(&p->sighand->siglock);
	/*
	 * Ensure all future scheduler executions will observe the new
	 * thread flag state for this process.
	 */
	synchronize_sched();
}
