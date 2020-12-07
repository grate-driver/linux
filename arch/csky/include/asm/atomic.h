/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_CSKY_ATOMIC_H
#define __ASM_CSKY_ATOMIC_H

#include <asm/barrier.h>

#define __atomic_acquire_fence() __smp_acquire_fence()
#define __atomic_release_fence() __smp_release_fence()

#include <asm-generic/atomic.h>

#endif /* __ASM_CSKY_ATOMIC_H */
