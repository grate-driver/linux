#ifndef __ASM_KASAN_H
#define __ASM_KASAN_H

#ifdef CONFIG_KASAN

#include <asm/kasan_def.h>
/*
 * Compiler uses shadow offset assuming that addresses start
 * from 0. Kernel addresses don't start from 0, so shadow
 * for kernel really starts from 'compiler's shadow offset' +
 * ('kernel address space start' >> KASAN_SHADOW_SCALE_SHIFT)
 */

extern void kasan_init(void);

#else
static inline void kasan_init(void) { }
#endif

#endif
