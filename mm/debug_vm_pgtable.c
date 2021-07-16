// SPDX-License-Identifier: GPL-2.0-only
/*
 * This kernel test validates architecture page table helpers and
 * accessors and helps in verifying their continued compliance with
 * expected generic MM semantics.
 *
 * Copyright (C) 2019 ARM Ltd.
 *
 * Author: Anshuman Khandual <anshuman.khandual@arm.com>
 */
#define pr_fmt(fmt) "debug_vm_pgtable: [%-25s]: " fmt, __func__

#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/hugetlb.h>
#include <linux/kernel.h>
#include <linux/kconfig.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mm_types.h>
#include <linux/module.h>
#include <linux/pfn_t.h>
#include <linux/printk.h>
#include <linux/pgtable.h>
#include <linux/random.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/start_kernel.h>
#include <linux/sched/mm.h>
#include <linux/io.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

/*
 * On s390 platform, the lower 4 bits are used to identify given page table
 * entry type. But these bits might affect the ability to clear entries with
 * pxx_clear() because of how dynamic page table folding works on s390. So
 * while loading up the entries do not change the lower 4 bits. It does not
 * have affect any other platform. Also avoid the 62nd bit on ppc64 that is
 * used to mark a pte entry.
 */
#define S390_SKIP_MASK		GENMASK(3, 0)
#if __BITS_PER_LONG == 64
#define PPC64_SKIP_MASK		GENMASK(62, 62)
#else
#define PPC64_SKIP_MASK		0x0
#endif
#define ARCH_SKIP_MASK (S390_SKIP_MASK | PPC64_SKIP_MASK)
#define RANDOM_ORVALUE (GENMASK(BITS_PER_LONG - 1, 0) & ~ARCH_SKIP_MASK)
#define RANDOM_NZVALUE	GENMASK(7, 0)

struct vm_pgtable_debug {
	struct mm_struct	*mm;
	struct vm_area_struct	*vma;

	pgd_t			*pgdp;
	p4d_t			*p4dp;
	pud_t			*pudp;
	pmd_t			*pmdp;
	pte_t			*ptep;

	p4d_t			*start_p4dp;
	pud_t			*start_pudp;
	pmd_t			*start_pmdp;
	pgtable_t		start_ptep;

	unsigned long		vaddr;
	pgprot_t		page_prot;
	pgprot_t		page_prot_none;

	unsigned long		pud_pfn;
	unsigned long		pmd_pfn;
	unsigned long		pte_pfn;

	unsigned long		fixed_pgd_pfn;
	unsigned long		fixed_p4d_pfn;
	unsigned long		fixed_pud_pfn;
	unsigned long		fixed_pmd_pfn;
	unsigned long		fixed_pte_pfn;
};

static void __init pte_basic_tests(struct vm_pgtable_debug *debug, int idx)
{
	pgprot_t prot = protection_map[idx];
	pte_t pte = pfn_pte(debug->fixed_pte_pfn, prot);
	unsigned long val = idx, *ptr = &val;

	pr_debug("Validating PTE basic (%pGv)\n", ptr);

	/*
	 * This test needs to be executed after the given page table entry
	 * is created with pfn_pte() to make sure that protection_map[idx]
	 * does not have the dirty bit enabled from the beginning. This is
	 * important for platforms like arm64 where (!PTE_RDONLY) indicate
	 * dirty bit being set.
	 */
	WARN_ON(pte_dirty(pte_wrprotect(pte)));

	WARN_ON(!pte_same(pte, pte));
	WARN_ON(!pte_young(pte_mkyoung(pte_mkold(pte))));
	WARN_ON(!pte_dirty(pte_mkdirty(pte_mkclean(pte))));
	WARN_ON(!pte_write(pte_mkwrite(pte_wrprotect(pte))));
	WARN_ON(pte_young(pte_mkold(pte_mkyoung(pte))));
	WARN_ON(pte_dirty(pte_mkclean(pte_mkdirty(pte))));
	WARN_ON(pte_write(pte_wrprotect(pte_mkwrite(pte))));
	WARN_ON(pte_dirty(pte_wrprotect(pte_mkclean(pte))));
	WARN_ON(!pte_dirty(pte_wrprotect(pte_mkdirty(pte))));
}

static void __init pte_advanced_tests(struct vm_pgtable_debug *debug)
{
	pte_t pte;

	/*
	 * Architectures optimize set_pte_at by avoiding TLB flush.
	 * This requires set_pte_at to be not used to update an
	 * existing pte entry. Clear pte before we do set_pte_at
	 */

	pr_debug("Validating PTE advanced\n");
	if (debug->pte_pfn == ULONG_MAX) {
		pr_debug("%s: Skipped\n", __func__);
		return;
	}

	pte = pfn_pte(debug->pte_pfn, debug->page_prot);
	set_pte_at(debug->mm, debug->vaddr, debug->ptep, pte);
	ptep_set_wrprotect(debug->mm, debug->vaddr, debug->ptep);
	pte = ptep_get(debug->ptep);
	WARN_ON(pte_write(pte));
	ptep_get_and_clear(debug->mm, debug->vaddr, debug->ptep);
	pte = ptep_get(debug->ptep);
	WARN_ON(!pte_none(pte));

	pte = pfn_pte(debug->pte_pfn, debug->page_prot);
	pte = pte_wrprotect(pte);
	pte = pte_mkclean(pte);
	set_pte_at(debug->mm, debug->vaddr, debug->ptep, pte);
	pte = pte_mkwrite(pte);
	pte = pte_mkdirty(pte);
	ptep_set_access_flags(debug->vma, debug->vaddr, debug->ptep, pte, 1);
	pte = ptep_get(debug->ptep);
	WARN_ON(!(pte_write(pte) && pte_dirty(pte)));
	ptep_get_and_clear_full(debug->mm, debug->vaddr, debug->ptep, 1);
	pte = ptep_get(debug->ptep);
	WARN_ON(!pte_none(pte));

	pte = pfn_pte(debug->pte_pfn, debug->page_prot);
	pte = pte_mkyoung(pte);
	set_pte_at(debug->mm, debug->vaddr, debug->ptep, pte);
	ptep_test_and_clear_young(debug->vma, debug->vaddr, debug->ptep);
	pte = ptep_get(debug->ptep);
	WARN_ON(pte_young(pte));
}

static void __init pte_savedwrite_tests(struct vm_pgtable_debug *debug)
{
	pte_t pte = pfn_pte(debug->fixed_pte_pfn, debug->page_prot_none);

	if (!IS_ENABLED(CONFIG_NUMA_BALANCING))
		return;

	pr_debug("Validating PTE saved write\n");
	WARN_ON(!pte_savedwrite(pte_mk_savedwrite(pte_clear_savedwrite(pte))));
	WARN_ON(pte_savedwrite(pte_clear_savedwrite(pte_mk_savedwrite(pte))));
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void __init pmd_basic_tests(struct vm_pgtable_debug *debug, int idx)
{
	pgprot_t prot = protection_map[idx];
	unsigned long val = idx, *ptr = &val;
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD basic (%pGv)\n", ptr);
	pmd = pfn_pmd(debug->fixed_pmd_pfn, prot);

	/*
	 * This test needs to be executed after the given page table entry
	 * is created with pfn_pmd() to make sure that protection_map[idx]
	 * does not have the dirty bit enabled from the beginning. This is
	 * important for platforms like arm64 where (!PTE_RDONLY) indicate
	 * dirty bit being set.
	 */
	WARN_ON(pmd_dirty(pmd_wrprotect(pmd)));


	WARN_ON(!pmd_same(pmd, pmd));
	WARN_ON(!pmd_young(pmd_mkyoung(pmd_mkold(pmd))));
	WARN_ON(!pmd_dirty(pmd_mkdirty(pmd_mkclean(pmd))));
	WARN_ON(!pmd_write(pmd_mkwrite(pmd_wrprotect(pmd))));
	WARN_ON(pmd_young(pmd_mkold(pmd_mkyoung(pmd))));
	WARN_ON(pmd_dirty(pmd_mkclean(pmd_mkdirty(pmd))));
	WARN_ON(pmd_write(pmd_wrprotect(pmd_mkwrite(pmd))));
	WARN_ON(pmd_dirty(pmd_wrprotect(pmd_mkclean(pmd))));
	WARN_ON(!pmd_dirty(pmd_wrprotect(pmd_mkdirty(pmd))));
	/*
	 * A huge page does not point to next level page table
	 * entry. Hence this must qualify as pmd_bad().
	 */
	WARN_ON(!pmd_bad(pmd_mkhuge(pmd)));
}

static void __init pmd_advanced_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd;
	unsigned long vaddr = (debug->vaddr & HPAGE_PMD_MASK);

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD advanced\n");
	if (debug->pmd_pfn == ULONG_MAX) {
		pr_debug("%s: Skipped\n", __func__);
		return;
	}

	pgtable_trans_huge_deposit(debug->mm, debug->pmdp, debug->start_ptep);

	pmd = pfn_pmd(debug->pmd_pfn, debug->page_prot);
	set_pmd_at(debug->mm, vaddr, debug->pmdp, pmd);
	pmdp_set_wrprotect(debug->mm, vaddr, debug->pmdp);
	pmd = READ_ONCE(*(debug->pmdp));
	WARN_ON(pmd_write(pmd));
	pmdp_huge_get_and_clear(debug->mm, vaddr, debug->pmdp);
	pmd = READ_ONCE(*(debug->pmdp));
	WARN_ON(!pmd_none(pmd));

	pmd = pfn_pmd(debug->pmd_pfn, debug->page_prot);
	pmd = pmd_wrprotect(pmd);
	pmd = pmd_mkclean(pmd);
	set_pmd_at(debug->mm, vaddr, debug->pmdp, pmd);
	pmd = pmd_mkwrite(pmd);
	pmd = pmd_mkdirty(pmd);
	pmdp_set_access_flags(debug->vma, vaddr, debug->pmdp, pmd, 1);
	pmd = READ_ONCE(*(debug->pmdp));
	WARN_ON(!(pmd_write(pmd) && pmd_dirty(pmd)));
	pmdp_huge_get_and_clear_full(debug->vma, vaddr, debug->pmdp, 1);
	pmd = READ_ONCE(*(debug->pmdp));
	WARN_ON(!pmd_none(pmd));

	pmd = pmd_mkhuge(pfn_pmd(debug->pmd_pfn, debug->page_prot));
	pmd = pmd_mkyoung(pmd);
	set_pmd_at(debug->mm, vaddr, debug->pmdp, pmd);
	pmdp_test_and_clear_young(debug->vma, vaddr, debug->pmdp);
	pmd = READ_ONCE(*(debug->pmdp));
	WARN_ON(pmd_young(pmd));

	/*  Clear the pte entries  */
	pmdp_huge_get_and_clear(debug->mm, vaddr, debug->pmdp);
	pgtable_trans_huge_withdraw(debug->mm, debug->pmdp);
}

static void __init pmd_leaf_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD leaf\n");
	pmd = pfn_pmd(debug->fixed_pmd_pfn, debug->page_prot);

	/*
	 * PMD based THP is a leaf entry.
	 */
	pmd = pmd_mkhuge(pmd);
	WARN_ON(!pmd_leaf(pmd));
}

static void __init pmd_savedwrite_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd;

	if (!IS_ENABLED(CONFIG_NUMA_BALANCING))
		return;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD saved write\n");
	pmd = pfn_pmd(debug->fixed_pmd_pfn, debug->page_prot_none);
	WARN_ON(!pmd_savedwrite(pmd_mk_savedwrite(pmd_clear_savedwrite(pmd))));
	WARN_ON(pmd_savedwrite(pmd_clear_savedwrite(pmd_mk_savedwrite(pmd))));
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
static void __init pud_basic_tests(struct vm_pgtable_debug *debug, int idx)
{
	pgprot_t prot = protection_map[idx];
	unsigned long val = idx, *ptr = &val;
	pud_t pud;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PUD basic (%pGv)\n", ptr);
	pud = pfn_pud(debug->fixed_pud_pfn, prot);

	/*
	 * This test needs to be executed after the given page table entry
	 * is created with pfn_pud() to make sure that protection_map[idx]
	 * does not have the dirty bit enabled from the beginning. This is
	 * important for platforms like arm64 where (!PTE_RDONLY) indicate
	 * dirty bit being set.
	 */
	WARN_ON(pud_dirty(pud_wrprotect(pud)));

	WARN_ON(!pud_same(pud, pud));
	WARN_ON(!pud_young(pud_mkyoung(pud_mkold(pud))));
	WARN_ON(!pud_dirty(pud_mkdirty(pud_mkclean(pud))));
	WARN_ON(pud_dirty(pud_mkclean(pud_mkdirty(pud))));
	WARN_ON(!pud_write(pud_mkwrite(pud_wrprotect(pud))));
	WARN_ON(pud_write(pud_wrprotect(pud_mkwrite(pud))));
	WARN_ON(pud_young(pud_mkold(pud_mkyoung(pud))));
	WARN_ON(pud_dirty(pud_wrprotect(pud_mkclean(pud))));
	WARN_ON(!pud_dirty(pud_wrprotect(pud_mkdirty(pud))));

	if (mm_pmd_folded(debug->mm))
		return;

	/*
	 * A huge page does not point to next level page table
	 * entry. Hence this must qualify as pud_bad().
	 */
	WARN_ON(!pud_bad(pud_mkhuge(pud)));
}

static void __init pud_advanced_tests(struct vm_pgtable_debug *debug)
{
	unsigned long vaddr = (debug->vaddr & HPAGE_PUD_MASK);
	pud_t pud;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PUD advanced\n");
	if (debug->pud_pfn == ULONG_MAX) {
		pr_debug("%s: Skipped\n", __func__);
		return;
	}

	pud = pfn_pud(debug->pud_pfn, debug->page_prot);
	set_pud_at(debug->mm, vaddr, debug->pudp, pud);
	pudp_set_wrprotect(debug->mm, vaddr, debug->pudp);
	pud = READ_ONCE(*(debug->pudp));
	WARN_ON(pud_write(pud));

#ifndef __PAGETABLE_PMD_FOLDED
	pudp_huge_get_and_clear(debug->mm, vaddr, debug->pudp);
	pud = READ_ONCE(*(debug->pudp));
	WARN_ON(!pud_none(pud));
#endif /* __PAGETABLE_PMD_FOLDED */
	pud = pfn_pud(debug->pud_pfn, debug->page_prot);
	pud = pud_wrprotect(pud);
	pud = pud_mkclean(pud);
	set_pud_at(debug->mm, vaddr, debug->pudp, pud);
	pud = pud_mkwrite(pud);
	pud = pud_mkdirty(pud);
	pudp_set_access_flags(debug->vma, vaddr, debug->pudp, pud, 1);
	pud = READ_ONCE(*(debug->pudp));
	WARN_ON(!(pud_write(pud) && pud_dirty(pud)));

#ifndef __PAGETABLE_PMD_FOLDED
	pudp_huge_get_and_clear_full(debug->mm, vaddr, debug->pudp, 1);
	pud = READ_ONCE(*(debug->pudp));
	WARN_ON(!pud_none(pud));
#endif /* __PAGETABLE_PMD_FOLDED */

	pud = pfn_pud(debug->pud_pfn, debug->page_prot);
	pud = pud_mkyoung(pud);
	set_pud_at(debug->mm, vaddr, debug->pudp, pud);
	pudp_test_and_clear_young(debug->vma, vaddr, debug->pudp);
	pud = READ_ONCE(*(debug->pudp));
	WARN_ON(pud_young(pud));

	pudp_huge_get_and_clear(debug->mm, vaddr, debug->pudp);
}

static void __init pud_leaf_tests(struct vm_pgtable_debug *debug)
{
	pud_t pud;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PUD leaf\n");
	pud = pfn_pud(debug->fixed_pud_pfn, debug->page_prot);
	/*
	 * PUD based THP is a leaf entry.
	 */
	pud = pud_mkhuge(pud);
	WARN_ON(!pud_leaf(pud));
}
#else  /* !CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
static void __init pud_basic_tests(struct vm_pgtable_debug *debug, int idx) { }
static void __init pud_advanced_tests(struct vm_pgtable_debug *debug) { }
static void __init pud_leaf_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
static void __init pmd_basic_tests(struct vm_pgtable_debug *debug, int idx) { }
static void __init pud_basic_tests(struct vm_pgtable_debug *debug, int idx) { }
static void __init pmd_advanced_tests(struct vm_pgtable_debug *debug) { }
static void __init pud_advanced_tests(struct vm_pgtable_debug *debug) { }
static void __init pmd_leaf_tests(struct vm_pgtable_debug *debug) { }
static void __init pud_leaf_tests(struct vm_pgtable_debug *debug) { }
static void __init pmd_savedwrite_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
static void __init pmd_huge_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd;

	if (!arch_vmap_pmd_supported(debug->page_prot))
		return;

	pr_debug("Validating PMD huge\n");
	/*
	 * X86 defined pmd_set_huge() verifies that the given
	 * PMD is not a populated non-leaf entry.
	 */
	WRITE_ONCE(*(debug->pmdp), __pmd(0));
	WARN_ON(!pmd_set_huge(debug->pmdp, __pfn_to_phys(debug->fixed_pmd_pfn),
			      debug->page_prot));
	WARN_ON(!pmd_clear_huge(debug->pmdp));
	pmd = READ_ONCE(*(debug->pmdp));
	WARN_ON(!pmd_none(pmd));
}

static void __init pud_huge_tests(struct vm_pgtable_debug *debug)
{
	pud_t pud;

	if (!arch_vmap_pud_supported(debug->page_prot))
		return;

	pr_debug("Validating PUD huge\n");
	/*
	 * X86 defined pud_set_huge() verifies that the given
	 * PUD is not a populated non-leaf entry.
	 */
	WRITE_ONCE(*(debug->pudp), __pud(0));
	WARN_ON(!pud_set_huge(debug->pudp, __pfn_to_phys(debug->fixed_pud_pfn),
			      debug->page_prot));
	WARN_ON(!pud_clear_huge(debug->pudp));
	pud = READ_ONCE(*(debug->pudp));
	WARN_ON(!pud_none(pud));
}
#else /* !CONFIG_HAVE_ARCH_HUGE_VMAP */
static void __init pmd_huge_tests(struct vm_pgtable_debug *debug) { }
static void __init pud_huge_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_HAVE_ARCH_HUGE_VMAP */

static void __init p4d_basic_tests(void)
{
	p4d_t p4d;

	pr_debug("Validating P4D basic\n");
	memset(&p4d, RANDOM_NZVALUE, sizeof(p4d_t));
	WARN_ON(!p4d_same(p4d, p4d));
}

static void __init pgd_basic_tests(void)
{
	pgd_t pgd;

	pr_debug("Validating PGD basic\n");
	memset(&pgd, RANDOM_NZVALUE, sizeof(pgd_t));
	WARN_ON(!pgd_same(pgd, pgd));
}

#ifndef __PAGETABLE_PUD_FOLDED
static void __init pud_clear_tests(struct vm_pgtable_debug *debug)
{
	pud_t pud = READ_ONCE(*(debug->pudp));

	if (mm_pmd_folded(debug->mm))
		return;

	pr_debug("Validating PUD clear\n");
	pud = __pud(pud_val(pud) | RANDOM_ORVALUE);
	WRITE_ONCE(*(debug->pudp), pud);
	pud_clear(debug->pudp);
	pud = READ_ONCE(*(debug->pudp));
	WARN_ON(!pud_none(pud));
}

static void __init pud_populate_tests(struct vm_pgtable_debug *debug)
{
	pud_t pud;

	if (mm_pmd_folded(debug->mm))
		return;

	pr_debug("Validating PUD populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pud_bad().
	 */
	pud_populate(debug->mm, debug->pudp, debug->start_pmdp);
	pud = READ_ONCE(*(debug->pudp));
	WARN_ON(pud_bad(pud));
}
#else  /* !__PAGETABLE_PUD_FOLDED */
static void __init pud_clear_tests(struct vm_pgtable_debug *debug) { }
static void __init pud_populate_tests(struct vm_pgtable_debug *debug) { }
#endif /* PAGETABLE_PUD_FOLDED */

#ifndef __PAGETABLE_P4D_FOLDED
static void __init p4d_clear_tests(struct vm_pgtable_debug *debug)
{
	p4d_t p4d = READ_ONCE(*(debug->p4dp));

	if (mm_pud_folded(debug->mm))
		return;

	pr_debug("Validating P4D clear\n");
	p4d = __p4d(p4d_val(p4d) | RANDOM_ORVALUE);
	WRITE_ONCE(*(debug->p4dp), p4d);
	p4d_clear(debug->p4dp);
	p4d = READ_ONCE(*(debug->p4dp));
	WARN_ON(!p4d_none(p4d));
}

static void __init p4d_populate_tests(struct vm_pgtable_debug *debug)
{
	p4d_t p4d;

	if (mm_pud_folded(debug->mm))
		return;

	pr_debug("Validating P4D populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as p4d_bad().
	 */
	pud_clear(debug->pudp);
	p4d_clear(debug->p4dp);
	p4d_populate(debug->mm, debug->p4dp, debug->start_pudp);
	p4d = READ_ONCE(*(debug->p4dp));
	WARN_ON(p4d_bad(p4d));
}

static void __init pgd_clear_tests(struct vm_pgtable_debug *debug)
{
	pgd_t pgd = READ_ONCE(*(debug->pgdp));

	if (mm_p4d_folded(debug->mm))
		return;

	pr_debug("Validating PGD clear\n");
	pgd = __pgd(pgd_val(pgd) | RANDOM_ORVALUE);
	WRITE_ONCE(*(debug->pgdp), pgd);
	pgd_clear(debug->pgdp);
	pgd = READ_ONCE(*(debug->pgdp));
	WARN_ON(!pgd_none(pgd));
}

static void __init pgd_populate_tests(struct vm_pgtable_debug *debug)
{
	pgd_t pgd;

	if (mm_p4d_folded(debug->mm))
		return;

	pr_debug("Validating PGD populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pgd_bad().
	 */
	p4d_clear(debug->p4dp);
	pgd_clear(debug->pgdp);
	pgd_populate(debug->mm, debug->pgdp, debug->start_p4dp);
	pgd = READ_ONCE(*(debug->pgdp));
	WARN_ON(pgd_bad(pgd));
}
#else  /* !__PAGETABLE_P4D_FOLDED */
static void __init p4d_clear_tests(struct vm_pgtable_debug *debug) { }
static void __init pgd_clear_tests(struct vm_pgtable_debug *debug) { }
static void __init p4d_populate_tests(struct vm_pgtable_debug *debug) { }
static void __init pgd_populate_tests(struct vm_pgtable_debug *debug) { }
#endif /* PAGETABLE_P4D_FOLDED */

static void __init pte_clear_tests(struct vm_pgtable_debug *debug)
{
	pte_t pte;

	pr_debug("Validating PTE clear\n");
	if (debug->pte_pfn == ULONG_MAX) {
		pr_debug("%s: Skipped\n", __func__);
		return;
	}

	pte = pfn_pte(debug->pte_pfn, debug->page_prot);
#ifndef CONFIG_RISCV
	pte = __pte(pte_val(pte) | RANDOM_ORVALUE);
#endif
	set_pte_at(debug->mm, debug->vaddr, debug->ptep, pte);
	barrier();
	pte_clear(debug->mm, debug->vaddr, debug->ptep);
	pte = ptep_get(debug->ptep);
	WARN_ON(!pte_none(pte));
}

static void __init pmd_clear_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd = READ_ONCE(*(debug->pmdp));

	pr_debug("Validating PMD clear\n");
	pmd = __pmd(pmd_val(pmd) | RANDOM_ORVALUE);
	WRITE_ONCE(*(debug->pmdp), pmd);
	pmd_clear(debug->pmdp);
	pmd = READ_ONCE(*(debug->pmdp));
	WARN_ON(!pmd_none(pmd));
}

static void __init pmd_populate_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd;

	pr_debug("Validating PMD populate\n");
	/*
	 * This entry points to next level page table page.
	 * Hence this must not qualify as pmd_bad().
	 */
	pmd_populate(debug->mm, debug->pmdp, debug->start_ptep);
	pmd = READ_ONCE(*(debug->pmdp));
	WARN_ON(pmd_bad(pmd));
}

static void __init pte_special_tests(struct vm_pgtable_debug *debug)
{
	pte_t pte = pfn_pte(debug->fixed_pte_pfn, debug->page_prot);

	if (!IS_ENABLED(CONFIG_ARCH_HAS_PTE_SPECIAL))
		return;

	pr_debug("Validating PTE special\n");
	WARN_ON(!pte_special(pte_mkspecial(pte)));
}

static void __init pte_protnone_tests(struct vm_pgtable_debug *debug)
{
	pte_t pte = pfn_pte(debug->fixed_pte_pfn, debug->page_prot_none);

	if (!IS_ENABLED(CONFIG_NUMA_BALANCING))
		return;

	pr_debug("Validating PTE protnone\n");
	WARN_ON(!pte_protnone(pte));
	WARN_ON(!pte_present(pte));
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void __init pmd_protnone_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd;

	if (!IS_ENABLED(CONFIG_NUMA_BALANCING))
		return;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD protnone\n");
	pmd = pmd_mkhuge(pfn_pmd(debug->fixed_pmd_pfn, debug->page_prot_none));
	WARN_ON(!pmd_protnone(pmd));
	WARN_ON(!pmd_present(pmd));
}
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
static void __init pmd_protnone_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifdef CONFIG_ARCH_HAS_PTE_DEVMAP
static void __init pte_devmap_tests(struct vm_pgtable_debug *debug)
{
	pte_t pte = pfn_pte(debug->fixed_pte_pfn, debug->page_prot);

	pr_debug("Validating PTE devmap\n");
	WARN_ON(!pte_devmap(pte_mkdevmap(pte)));
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void __init pmd_devmap_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD devmap\n");
	pmd = pfn_pmd(debug->fixed_pmd_pfn, debug->page_prot);
	WARN_ON(!pmd_devmap(pmd_mkdevmap(pmd)));
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
static void __init pud_devmap_tests(struct vm_pgtable_debug *debug)
{
	pud_t pud;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PUD devmap\n");
	pud = pfn_pud(debug->fixed_pud_pfn, debug->page_prot);
	WARN_ON(!pud_devmap(pud_mkdevmap(pud)));
}
#else  /* !CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
static void __init pud_devmap_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
#else  /* CONFIG_TRANSPARENT_HUGEPAGE */
static void __init pmd_devmap_tests(struct vm_pgtable_debug *debug) { }
static void __init pud_devmap_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#else
static void __init pte_devmap_tests(struct vm_pgtable_debug *debug) { }
static void __init pmd_devmap_tests(struct vm_pgtable_debug *debug) { }
static void __init pud_devmap_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_ARCH_HAS_PTE_DEVMAP */

static void __init pte_soft_dirty_tests(struct vm_pgtable_debug *debug)
{
	pte_t pte = pfn_pte(debug->fixed_pte_pfn, debug->page_prot);

	if (!IS_ENABLED(CONFIG_MEM_SOFT_DIRTY))
		return;

	pr_debug("Validating PTE soft dirty\n");
	WARN_ON(!pte_soft_dirty(pte_mksoft_dirty(pte)));
	WARN_ON(pte_soft_dirty(pte_clear_soft_dirty(pte)));
}

static void __init pte_swap_soft_dirty_tests(struct vm_pgtable_debug *debug)
{
	pte_t pte = pfn_pte(debug->fixed_pte_pfn, debug->page_prot);

	if (!IS_ENABLED(CONFIG_MEM_SOFT_DIRTY))
		return;

	pr_debug("Validating PTE swap soft dirty\n");
	WARN_ON(!pte_swp_soft_dirty(pte_swp_mksoft_dirty(pte)));
	WARN_ON(pte_swp_soft_dirty(pte_swp_clear_soft_dirty(pte)));
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void __init pmd_soft_dirty_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd;

	if (!IS_ENABLED(CONFIG_MEM_SOFT_DIRTY))
		return;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD soft dirty\n");
	pmd = pfn_pmd(debug->fixed_pmd_pfn, debug->page_prot);
	WARN_ON(!pmd_soft_dirty(pmd_mksoft_dirty(pmd)));
	WARN_ON(pmd_soft_dirty(pmd_clear_soft_dirty(pmd)));
}

static void __init pmd_swap_soft_dirty_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd;

	if (!IS_ENABLED(CONFIG_MEM_SOFT_DIRTY) ||
		!IS_ENABLED(CONFIG_ARCH_ENABLE_THP_MIGRATION))
		return;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD swap soft dirty\n");
	pmd = pfn_pmd(debug->fixed_pmd_pfn, debug->page_prot);
	WARN_ON(!pmd_swp_soft_dirty(pmd_swp_mksoft_dirty(pmd)));
	WARN_ON(pmd_swp_soft_dirty(pmd_swp_clear_soft_dirty(pmd)));
}
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
static void __init pmd_soft_dirty_tests(struct vm_pgtable_debug *debug) { }
static void __init pmd_swap_soft_dirty_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

static void __init pte_swap_tests(struct vm_pgtable_debug *debug)
{
	swp_entry_t swp;
	pte_t pte;

	pr_debug("Validating PTE swap\n");
	pte = pfn_pte(debug->fixed_pte_pfn, debug->page_prot);
	swp = __pte_to_swp_entry(pte);
	pte = __swp_entry_to_pte(swp);
	WARN_ON(debug->fixed_pte_pfn != pte_pfn(pte));
}

#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
static void __init pmd_swap_tests(struct vm_pgtable_debug *debug)
{
	swp_entry_t swp;
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD swap\n");
	pmd = pfn_pmd(debug->fixed_pmd_pfn, debug->page_prot);
	swp = __pmd_to_swp_entry(pmd);
	pmd = __swp_entry_to_pmd(swp);
	WARN_ON(debug->fixed_pmd_pfn != pmd_pfn(pmd));
}
#else  /* !CONFIG_ARCH_ENABLE_THP_MIGRATION */
static void __init pmd_swap_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_ARCH_ENABLE_THP_MIGRATION */

static void __init swap_migration_tests(struct vm_pgtable_debug *debug)
{
	struct page *page;
	swp_entry_t swp;

	if (!IS_ENABLED(CONFIG_MIGRATION))
		return;

	pr_debug("Validating swap migration\n");
	/*
	 * swap_migration_tests() requires a dedicated page as it needs to
	 * be locked before creating a migration entry from it. Locking the
	 * page that actually maps kernel text ('start_kernel') can be real
	 * problematic. Lets allocate a dedicated page explicitly for this
	 * purpose that will be freed subsequently.
	 */
	page = (debug->pte_pfn != ULONG_MAX) ?
	       pfn_to_page(debug->pte_pfn) : NULL;
	if (!page) {
		pr_err("no page available\n");
		return;
	}

	/*
	 * make_migration_entry() expects given page to be
	 * locked, otherwise it stumbles upon a BUG_ON().
	 */
	__SetPageLocked(page);
	swp = make_writable_migration_entry(page_to_pfn(page));
	WARN_ON(!is_migration_entry(swp));
	WARN_ON(!is_writable_migration_entry(swp));

	swp = make_readable_migration_entry(swp_offset(swp));
	WARN_ON(!is_migration_entry(swp));
	WARN_ON(is_writable_migration_entry(swp));

	swp = make_readable_migration_entry(page_to_pfn(page));
	WARN_ON(!is_migration_entry(swp));
	WARN_ON(is_writable_migration_entry(swp));
	__ClearPageLocked(page);
}

#ifdef CONFIG_HUGETLB_PAGE
static void __init hugetlb_basic_tests(struct vm_pgtable_debug *debug)
{
	struct page *page;
	pte_t pte;

	pr_debug("Validating HugeTLB basic\n");
	/*
	 * Accessing the page associated with the pfn is safe here,
	 * as it was previously derived from a real kernel symbol.
	 */
	page = pfn_to_page(debug->fixed_pmd_pfn);
	pte = mk_huge_pte(page, debug->page_prot);

	WARN_ON(!huge_pte_dirty(huge_pte_mkdirty(pte)));
	WARN_ON(!huge_pte_write(huge_pte_mkwrite(huge_pte_wrprotect(pte))));
	WARN_ON(huge_pte_write(huge_pte_wrprotect(huge_pte_mkwrite(pte))));

#ifdef CONFIG_ARCH_WANT_GENERAL_HUGETLB
	pte = pfn_pte(debug->fixed_pmd_pfn, debug->page_prot);

	WARN_ON(!pte_huge(pte_mkhuge(pte)));
#endif /* CONFIG_ARCH_WANT_GENERAL_HUGETLB */
}
#else  /* !CONFIG_HUGETLB_PAGE */
static void __init hugetlb_basic_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_HUGETLB_PAGE */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void __init pmd_thp_tests(struct vm_pgtable_debug *debug)
{
	pmd_t pmd;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PMD based THP\n");
	/*
	 * pmd_trans_huge() and pmd_present() must return positive after
	 * MMU invalidation with pmd_mkinvalid(). This behavior is an
	 * optimization for transparent huge page. pmd_trans_huge() must
	 * be true if pmd_page() returns a valid THP to avoid taking the
	 * pmd_lock when others walk over non transhuge pmds (i.e. there
	 * are no THP allocated). Especially when splitting a THP and
	 * removing the present bit from the pmd, pmd_trans_huge() still
	 * needs to return true. pmd_present() should be true whenever
	 * pmd_trans_huge() returns true.
	 */
	pmd = pfn_pmd(debug->fixed_pmd_pfn, debug->page_prot);
	WARN_ON(!pmd_trans_huge(pmd_mkhuge(pmd)));

#ifndef __HAVE_ARCH_PMDP_INVALIDATE
	WARN_ON(!pmd_trans_huge(pmd_mkinvalid(pmd_mkhuge(pmd))));
	WARN_ON(!pmd_present(pmd_mkinvalid(pmd_mkhuge(pmd))));
#endif /* __HAVE_ARCH_PMDP_INVALIDATE */
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
static void __init pud_thp_tests(struct vm_pgtable_debug *debug)
{
	pud_t pud;

	if (!has_transparent_hugepage())
		return;

	pr_debug("Validating PUD based THP\n");
	pud = pfn_pud(debug->fixed_pud_pfn, debug->page_prot);
	WARN_ON(!pud_trans_huge(pud_mkhuge(pud)));

	/*
	 * pud_mkinvalid() has been dropped for now. Enable back
	 * these tests when it comes back with a modified pud_present().
	 *
	 * WARN_ON(!pud_trans_huge(pud_mkinvalid(pud_mkhuge(pud))));
	 * WARN_ON(!pud_present(pud_mkinvalid(pud_mkhuge(pud))));
	 */
}
#else  /* !CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
static void __init pud_thp_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
#else  /* !CONFIG_TRANSPARENT_HUGEPAGE */
static void __init pmd_thp_tests(struct vm_pgtable_debug *debug) { }
static void __init pud_thp_tests(struct vm_pgtable_debug *debug) { }
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

static unsigned long __init get_random_vaddr(void)
{
	unsigned long random_vaddr, random_pages, total_user_pages;

	total_user_pages = (TASK_SIZE - FIRST_USER_ADDRESS) / PAGE_SIZE;

	random_pages = get_random_long() % total_user_pages;
	random_vaddr = FIRST_USER_ADDRESS + random_pages * PAGE_SIZE;

	return random_vaddr;
}

static void __init free_mem(struct vm_pgtable_debug *debug)
{
	struct page *page = NULL;
	int order = 0;

	/* Free (huge) page */
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
	if (has_transparent_hugepage() &&
	    debug->pud_pfn != ULONG_MAX) {
		page = pfn_to_page(debug->pud_pfn);
		order = HPAGE_PUD_SHIFT - PAGE_SHIFT;
	}
#endif

	if (has_transparent_hugepage() &&
	    debug->pmd_pfn != ULONG_MAX && !page) {
		page = pfn_to_page(debug->pmd_pfn);
		order = HPAGE_PMD_ORDER;
	}
#endif

	if (debug->pte_pfn != ULONG_MAX && !page) {
		page = pfn_to_page(debug->pte_pfn);
		order = 0;
	}

	if (page)
		__free_pages(page, order);

	/* Free page table */
	if (debug->start_ptep) {
		pte_free(debug->mm, debug->start_ptep);
		mm_dec_nr_ptes(debug->mm);
	}

	if (debug->start_pmdp) {
		pmd_free(debug->mm, debug->start_pmdp);
		mm_dec_nr_pmds(debug->mm);
	}

	if (debug->start_pudp) {
		pud_free(debug->mm, debug->start_pudp);
		mm_dec_nr_puds(debug->mm);
	}

	if (debug->start_p4dp)
		p4d_free(debug->mm, debug->p4dp);

	/* Free vma and mm struct */
	if (debug->vma)
		vm_area_free(debug->vma);
	if (debug->mm)
		mmdrop(debug->mm);
}

static int __init alloc_mem(struct vm_pgtable_debug *debug)
{
	struct page *page = NULL;
	phys_addr_t phys;
	int ret = 0;

	/*
	 * Initialize the debugging data. For @page_prot, please refer
	 * Documentation/vm/arch_pgtable_helpers.rst for the semantics
	 * expectations that are being validated here. All future changes
	 * in here or the documentation need to be in sync.
	 */
	debug->mm             = NULL;
	debug->vma            = NULL;
	debug->pgdp           = NULL;
	debug->p4dp           = NULL;
	debug->pudp           = NULL;
	debug->pmdp           = NULL;
	debug->ptep           = NULL;
	debug->start_p4dp     = NULL;
	debug->start_pudp     = NULL;
	debug->start_pmdp     = NULL;
	debug->start_ptep     = NULL;
	debug->vaddr          = 0UL;
	debug->page_prot      = vm_get_page_prot(VM_READ | VM_WRITE | VM_EXEC);
	debug->page_prot_none = __P000;
	debug->pud_pfn        = ULONG_MAX;
	debug->pmd_pfn        = ULONG_MAX;
	debug->pte_pfn        = ULONG_MAX;
	debug->fixed_pgd_pfn  = ULONG_MAX;
	debug->fixed_p4d_pfn  = ULONG_MAX;
	debug->fixed_pud_pfn  = ULONG_MAX;
	debug->fixed_pmd_pfn  = ULONG_MAX;
	debug->fixed_pte_pfn  = ULONG_MAX;

	/* Allocate mm and vma */
	debug->mm = mm_alloc();
	if (!debug->mm) {
		pr_warn("Failed to allocate mm struct\n");
		ret = -ENOMEM;
		goto error;
	}

	debug->vma = vm_area_alloc(debug->mm);
	if (!debug->vma) {
		pr_warn("Failed to allocate vma\n");
		ret = -ENOMEM;
		goto error;
	}

	/* Figure out the virtual address and allocate page table entries */
	debug->vaddr = get_random_vaddr();
	debug->pgdp = pgd_offset(debug->mm, debug->vaddr);
	debug->p4dp = p4d_alloc(debug->mm, debug->pgdp, debug->vaddr);
	debug->pudp = debug->p4dp ?
		      pud_alloc(debug->mm, debug->p4dp, debug->vaddr) : NULL;
	debug->pmdp = debug->pudp ?
		      pmd_alloc(debug->mm, debug->pudp, debug->vaddr) : NULL;
	debug->ptep = debug->pmdp ?
		      pte_alloc_map(debug->mm, debug->pmdp, debug->vaddr) : NULL;
	if (!debug->ptep) {
		pr_warn("Failed to allocate page table\n");
		ret = -ENOMEM;
		goto error;
	}

	/*
	 * The above page table entries will be modified. Lets save the
	 * page table entries so that they can be released when the tests
	 * are completed.
	 */
	debug->start_p4dp = p4d_offset(debug->pgdp, 0UL);
	debug->start_pudp = pud_offset(debug->p4dp, 0UL);
	debug->start_pmdp = pmd_offset(debug->pudp, 0UL);
	debug->start_ptep = pmd_pgtable(*(debug->pmdp));

	/*
	 * Figure out the fixed addresses, which are all around the kernel
	 * symbol (@start_kernel). The corresponding PFNs might be invalid,
	 * but it's fine as the following tests won't access the pages.
	 */
	phys = __pa_symbol(&start_kernel);
	debug->fixed_pgd_pfn = __phys_to_pfn(phys & PGDIR_MASK);
	debug->fixed_p4d_pfn = __phys_to_pfn(phys & P4D_MASK);
	debug->fixed_pud_pfn = __phys_to_pfn(phys & PUD_MASK);
	debug->fixed_pmd_pfn = __phys_to_pfn(phys & PMD_MASK);
	debug->fixed_pte_pfn = __phys_to_pfn(phys & PAGE_MASK);

	/*
	 * Allocate (huge) pages because some of the tests need to access
	 * the data in the pages. The corresponding tests will be skipped
	 * if we fail to allocate (huge) pages.
	 */
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
	if (has_transparent_hugepage()) {
		page = alloc_pages(GFP_KERNEL, HPAGE_PUD_SHIFT - PAGE_SHIFT);
		if (page)
			debug->pud_pfn = page_to_pfn(page);
	}
#endif

	if (has_transparent_hugepage()) {
		page = page ? page : alloc_pages(GFP_KERNEL, HPAGE_PMD_ORDER);
		if (page)
			debug->pmd_pfn = page_to_pfn(page);
	}
#endif

	page = page ? page : alloc_pages(GFP_KERNEL, 0);
	if (page)
		debug->pte_pfn = page_to_pfn(page);

	return 0;

error:
	free_mem(debug);
	return ret;
}

static int __init debug_vm_pgtable(void)
{
	struct vm_pgtable_debug debug;
	spinlock_t *ptl = NULL;
	int idx, ret;

	pr_info("Validating architecture page table helpers\n");
	ret = alloc_mem(&debug);
	if (ret)
		return ret;

	/*
	 * Iterate over the protection_map[] to make sure that all
	 * the basic page table transformation validations just hold
	 * true irrespective of the starting protection value for a
	 * given page table entry.
	 */
	for (idx = 0; idx < ARRAY_SIZE(protection_map); idx++) {
		pte_basic_tests(&debug, idx);
		pmd_basic_tests(&debug, idx);
		pud_basic_tests(&debug, idx);
	}

	/*
	 * Both P4D and PGD level tests are very basic which do not
	 * involve creating page table entries from the protection
	 * value and the given pfn. Hence just keep them out from
	 * the above iteration for now to save some test execution
	 * time.
	 */
	p4d_basic_tests();
	pgd_basic_tests();
	hugetlb_basic_tests(&debug);

	pmd_leaf_tests(&debug);
	pud_leaf_tests(&debug);

	pte_savedwrite_tests(&debug);
	pmd_savedwrite_tests(&debug);

	pte_special_tests(&debug);
	pte_protnone_tests(&debug);
	pmd_protnone_tests(&debug);

	pte_devmap_tests(&debug);
	pmd_devmap_tests(&debug);
	pud_devmap_tests(&debug);

	pte_soft_dirty_tests(&debug);
	pmd_soft_dirty_tests(&debug);
	pte_swap_soft_dirty_tests(&debug);
	pmd_swap_soft_dirty_tests(&debug);

	pte_swap_tests(&debug);
	pmd_swap_tests(&debug);

	swap_migration_tests(&debug);

	pmd_thp_tests(&debug);
	pud_thp_tests(&debug);

	/*
	 * Page table modifying tests. They need to hold
	 * proper page table lock.
	 */
	ptl = pte_lockptr(debug.mm, debug.pmdp);
	spin_lock(ptl);
	pte_clear_tests(&debug);
	pte_advanced_tests(&debug);
	spin_unlock(ptl);

	ptl = pmd_lock(debug.mm, debug.pmdp);
	pmd_clear_tests(&debug);
	pmd_advanced_tests(&debug);
	pmd_huge_tests(&debug);
	pmd_populate_tests(&debug);
	spin_unlock(ptl);

	ptl = pud_lock(debug.mm, debug.pudp);
	pud_clear_tests(&debug);
	pud_advanced_tests(&debug);
	pud_huge_tests(&debug);
	pud_populate_tests(&debug);
	spin_unlock(ptl);

	spin_lock(&(debug.mm->page_table_lock));
	p4d_clear_tests(&debug);
	pgd_clear_tests(&debug);
	p4d_populate_tests(&debug);
	pgd_populate_tests(&debug);
	spin_unlock(&(debug.mm->page_table_lock));

	free_mem(&debug);
	return 0;
}
late_initcall(debug_vm_pgtable);
