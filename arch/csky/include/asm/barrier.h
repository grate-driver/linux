/* SPDX-License-Identifier: GPL-2.0 */
// Copyright (C) 2018 Hangzhou C-SKY Microsystems co.,ltd.

#ifndef __ASM_CSKY_BARRIER_H
#define __ASM_CSKY_BARRIER_H

#ifndef __ASSEMBLY__

#define nop()	asm volatile ("nop\n":::"memory")

/*
 * sync:        completion barrier, all sync.xx instructions
 *              guarantee the last response recieved by bus transaction
 *              made by ld/st instructions before sync.s
 * sync.s:      inherit from sync, but also shareable to other cores
 * sync.i:      inherit from sync, but also flush cpu pipeline
 * sync.is:     the same with sync.i + sync.s
 *
 *
 * bar.brwarws: ordering barrier for all load/store instructions
 *              before/after it and share to other harts
 *
 * |31|30 26|25 21|20 16|15  10|9   5|4           0|
 *  1  10000 s0000 00000 100001	00001 0 bw br aw ar
 *
 * b: before
 * a: after
 * r: read
 * w: write
 * s: share to other harts
 *
 * Here are all combinations:
 *
 * bar.brws
 * bar.brs
 * bar.bws
 * bar.arws
 * bar.ars
 * bar.aws
 * bar.brwarws
 * bar.brarws
 * bar.bwarws
 * bar.brwars
 * bar.brwaws
 * bar.brars
 * bar.bwaws
 */

#ifdef CONFIG_CPU_HAS_CACHEV2
#define mb()		asm volatile ("sync.s\n":::"memory")

#ifdef CONFIG_SMP

#define __bar_brws()	asm volatile (".long 0x842cc200\n":::"memory")
#define __bar_brs()	asm volatile (".long 0x8424c200\n":::"memory")
#define __bar_bws()	asm volatile (".long 0x8428c200\n":::"memory")
#define __bar_arws()	asm volatile (".long 0x8423c200\n":::"memory")
#define __bar_ars()	asm volatile (".long 0x8421c200\n":::"memory")
#define __bar_aws()	asm volatile (".long 0x8422c200\n":::"memory")
#define __bar_brwarws()	asm volatile (".long 0x842fc200\n":::"memory")
#define __bar_brarws()	asm volatile (".long 0x8427c200\n":::"memory")
#define __bar_bwarws()	asm volatile (".long 0x842bc200\n":::"memory")
#define __bar_brwars()	asm volatile (".long 0x842dc200\n":::"memory")
#define __bar_brwaws()	asm volatile (".long 0x842ec200\n":::"memory")
#define __bar_brars()	asm volatile (".long 0x8425c200\n":::"memory")
#define __bar_brars()	asm volatile (".long 0x8425c200\n":::"memory")
#define __bar_bwaws()	asm volatile (".long 0x842ac200\n":::"memory")

#define __smp_mb()	__bar_brwarws()
#define __smp_rmb()	__bar_brars()
#define __smp_wmb()	__bar_bwaws()

#define ACQUIRE_FENCE		".long 0x8427c200\n"
#define __smp_acquire_fence()	__bar_brarws()
#define __smp_release_fence()	__bar_brwaws()

#endif /* CONFIG_SMP */

#define sync_is()	asm volatile ("sync.is\n":::"memory")

#else /* !CONFIG_CPU_HAS_CACHEV2 */
#define mb()		asm volatile ("sync\n":::"memory")
#endif

#include <asm-generic/barrier.h>

#endif /* __ASSEMBLY__ */
#endif /* __ASM_CSKY_BARRIER_H */
