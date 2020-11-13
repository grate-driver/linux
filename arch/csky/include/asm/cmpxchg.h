/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __ASM_CSKY_CMPXCHG_H
#define __ASM_CSKY_CMPXCHG_H

#ifdef CONFIG_CPU_HAS_LDSTEX
#include <asm/barrier.h>

extern void __bad_xchg(void);

#define __xchg_relaxed(new, ptr, size)				\
({								\
	__typeof__(ptr) __ptr = (ptr);				\
	__typeof__(new) __new = (new);				\
	__typeof__(*(ptr)) __ret = 0;				\
	unsigned long tmp, tmp2, align, addr;			\
	switch (size) {						\
	case 2:							\
		align = ((unsigned long) __ptr & 0x3);		\
		addr = ((unsigned long) __ptr & ~0x3);		\
		if (align) {					\
		asm volatile (					\
		"1:	ldex.w		%0, (%4) \n"		\
		"	mov		%1, %0   \n"		\
		"	lsli		%1, 16   \n"		\
		"	lsri		%1, 16   \n"		\
		"	mov		%2, %3   \n"		\
		"	lsli		%2, 16   \n"		\
		"	or		%1, %2   \n"		\
		"	stex.w		%1, (%4) \n"		\
		"	bez		%1, 1b   \n"		\
		"	lsri		%0, 16   \n"		\
			: "=&r" (__ret), "=&r" (tmp),		\
			  "=&r" (tmp2)				\
			: "r" (__new), "r"(addr)		\
			:);					\
		} else {					\
		asm volatile (					\
		"1:	ldex.w		%0, (%3) \n"		\
		"	mov		%1, %0   \n"		\
		"	lsri		%1, 16   \n"		\
		"	lsli		%1, 16   \n"		\
		"	or		%1, %2   \n"		\
		"	stex.w		%1, (%3) \n"		\
		"	bez		%1, 1b   \n"		\
		"	lsli		%0, 16   \n"		\
		"	lsri		%0, 16   \n"		\
			: "=&r" (__ret), "=&r" (tmp)		\
			: "r" (__new), "r"(addr)		\
			:);					\
		}						\
		break;						\
	case 4:							\
		asm volatile (					\
		"1:	ldex.w		%0, (%3) \n"		\
		"	mov		%1, %2   \n"		\
		"	stex.w		%1, (%3) \n"		\
		"	bez		%1, 1b   \n"		\
			: "=&r" (__ret), "=&r" (tmp)		\
			: "r" (__new), "r"(__ptr)		\
			:);					\
		break;						\
	default:						\
		__bad_xchg();					\
	}							\
	__ret;							\
})

#define xchg_relaxed(ptr, x)					\
({								\
	__xchg_relaxed((x), (ptr), sizeof(*(ptr)));		\
})

#define xchg_acquire(ptr, x)					\
({								\
	__typeof__(*(ptr)) __ret;				\
	__ret = xchg_relaxed(ptr, x);				\
	__smp_acquire_fence();					\
	__ret;							\
})

#define xchg_release(ptr, x)					\
({								\
	__smp_release_fence();					\
	xchg_relaxed(ptr, x);					\
})

#define xchg(ptr, x)						\
({								\
	__smp_release_fence();					\
	xchg_acquire(ptr, x);					\
})

#define __cmpxchg_relaxed(ptr, old, new, size)			\
({								\
	__typeof__(ptr) __ptr = (ptr);				\
	__typeof__(new) __new = (new);				\
	__typeof__(new) __tmp;					\
	__typeof__(old) __old = (old);				\
	__typeof__(*(ptr)) __ret = 0;				\
	switch (size) {						\
	case 4:							\
		asm volatile (					\
		"1:	ldex.w		%0, (%3) \n"		\
		"	cmpne		%0, %4   \n"		\
		"	bt		2f       \n"		\
		"	mov		%1, %2   \n"		\
		"	stex.w		%1, (%3) \n"		\
		"	bez		%1, 1b   \n"		\
		"2:				 \n"		\
			: "=&r" (__ret), "=&r" (__tmp)		\
			: "r" (__new), "r"(__ptr), "r"(__old)	\
			:);					\
		break;						\
	default:						\
		__bad_xchg();					\
	}							\
	__ret;							\
})

#define cmpxchg_relaxed(ptr, o, n)				\
	(__cmpxchg_relaxed((ptr), (o), (n), sizeof(*(ptr))))

#define cmpxchg_release(ptr, o, n)				\
({								\
	__smp_release_fence();					\
	cmpxchg_relaxed(ptr, o, n);				\
})

#define __cmpxchg_acquire(ptr, old, new, size)			\
({								\
	__typeof__(ptr) __ptr = (ptr);				\
	__typeof__(new) __new = (new);				\
	__typeof__(new) __tmp;					\
	__typeof__(old) __old = (old);				\
	__typeof__(*(ptr)) __ret = 0;				\
	switch (size) {						\
	case 4:							\
		asm volatile (					\
		"1:	ldex.w		%0, (%3) \n"		\
		"	cmpne		%0, %4   \n"		\
		"	bt		2f       \n"		\
		"	mov		%1, %2   \n"		\
		"	stex.w		%1, (%3) \n"		\
		"	bez		%1, 1b   \n"		\
		ACQUIRE_FENCE					\
		"2:				 \n"		\
			: "=&r" (__ret), "=&r" (__tmp)		\
			: "r" (__new), "r"(__ptr), "r"(__old)	\
			:);					\
		break;						\
	default:						\
		__bad_xchg();					\
	}							\
	__ret;							\
})

#define cmpxchg_acquire(ptr, o, n)				\
	(__cmpxchg_acquire((ptr), (o), (n), sizeof(*(ptr))))

#define cmpxchg(ptr, o, n)					\
({								\
	__typeof__(*(ptr)) __ret;				\
	__smp_release_fence();					\
	__ret = cmpxchg_acquire(ptr, o, n);			\
	__ret;							\
})

#else
#include <asm-generic/cmpxchg.h>
#endif

#endif /* __ASM_CSKY_CMPXCHG_H */
