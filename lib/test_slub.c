// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test cases for slub facility.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include "../mm/slab.h"

#include "../tools/testing/selftests/kselftest_module.h"


KSTM_MODULE_GLOBALS();


static void __init validate_result(struct kmem_cache *s, int expected_errors)
{
	int errors = 0;

	validate_slab_cache(s, &errors);
	KSTM_CHECK_ZERO(errors - expected_errors);
}

static void __init test_clobber_zone(void)
{
	struct kmem_cache *s = kmem_cache_create("TestSlub_RZ_alloc", 64, 0,
				SLAB_RED_ZONE, NULL);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	p[64] = 0x12;
	pr_err("1. kmem_cache: Clobber Redzone 0x12->0x%p\n", p + 64);

	validate_result(s, 1);
	kmem_cache_free(s, p);
	kmem_cache_destroy(s);
}

static void __init test_next_pointer(void)
{
	struct kmem_cache *s = kmem_cache_create("TestSlub_next_ptr_free", 64, 0,
				SLAB_RED_ZONE, NULL);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kmem_cache_free(s, p);
	p[s->offset] = 0x12;
	pr_err("1. kmem_cache: Clobber next pointer 0x34 -> -0x%p\n", p);

	validate_result(s, 1);
	kmem_cache_destroy(s);
}

static void __init test_first_word(void)
{
	struct kmem_cache *s = kmem_cache_create("TestSlub_1th_word_free", 64, 0,
				SLAB_POISON, NULL);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kmem_cache_free(s, p);
	*p = 0x78;
	pr_err("2. kmem_cache: Clobber first word 0x78->0x%p\n", p);

	validate_result(s, 1);
	kmem_cache_destroy(s);
}

static void __init test_clobber_50th_byte(void)
{
	struct kmem_cache *s = kmem_cache_create("TestSlub_50th_word_free", 64, 0,
				SLAB_POISON, NULL);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kmem_cache_free(s, p);
	p[50] = 0x9a;
	pr_err("3. kmem_cache: Clobber 50th byte 0x9a->0x%p\n", p);

	validate_result(s, 1);
	kmem_cache_destroy(s);
}

static void __init test_clobber_redzone_free(void)
{
	struct kmem_cache *s = kmem_cache_create("TestSlub_RZ_free", 64, 0,
				SLAB_RED_ZONE, NULL);
	u8 *p = kmem_cache_alloc(s, GFP_KERNEL);

	kmem_cache_free(s, p);
	p[64] = 0xab;
	pr_err("4. kmem_cache: Clobber redzone 0xab->0x%p\n", p);

	validate_result(s, 1);
	kmem_cache_destroy(s);
}

static void __init resiliency_test(void)
{

	BUILD_BUG_ON(KMALLOC_MIN_SIZE > 16 || KMALLOC_SHIFT_HIGH < 10);

	pr_err("SLUB resiliency testing\n");
	pr_err("-----------------------\n");
	pr_err("A. Corruption after allocation\n");

	test_clobber_zone();

	pr_err("\nB. Corruption after free\n");

	test_next_pointer();
	test_first_word();
	test_clobber_50th_byte();
	test_clobber_redzone_free();
}


static void __init selftest(void)
{
	resiliency_test();
}


KSTM_MODULE_LOADERS(test_slub);
MODULE_LICENSE("GPL");
