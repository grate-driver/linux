// SPDX-License-Identifier: GPL-2.0

#include <linux/bug.h>
#include <linux/build_bug.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>

void rust_helper_BUG(void)
{
	BUG();
}

int rust_helper_access_ok(const void __user *addr, unsigned long n)
{
	return access_ok(addr, n);
}

unsigned long rust_helper_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	return copy_from_user(to, from, n);
}

unsigned long rust_helper_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	return copy_to_user(to, from, n);
}

void rust_helper_spin_lock_init(spinlock_t *lock, const char *name,
				struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_SPINLOCK
	__spin_lock_init(lock, name, key);
#else
	spin_lock_init(lock);
#endif
}
EXPORT_SYMBOL(rust_helper_spin_lock_init);

void rust_helper_spin_lock(spinlock_t *lock)
{
	spin_lock(lock);
}
EXPORT_SYMBOL(rust_helper_spin_lock);

void rust_helper_spin_unlock(spinlock_t *lock)
{
	spin_unlock(lock);
}
EXPORT_SYMBOL(rust_helper_spin_unlock);

void rust_helper_init_wait(struct wait_queue_entry *wq_entry)
{
	init_wait(wq_entry);
}
EXPORT_SYMBOL(rust_helper_init_wait);

int rust_helper_signal_pending(void)
{
	return signal_pending(current);
}
EXPORT_SYMBOL(rust_helper_signal_pending);

// See https://github.com/rust-lang/rust-bindgen/issues/1671
static_assert(__builtin_types_compatible_p(size_t, uintptr_t),
	"size_t must match uintptr_t, what architecture is this??");
