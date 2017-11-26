/*
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * This code is based in part on work published here:
 *
 *	https://github.com/IAIK/KAISER
 *
 * The original work was written by and and signed off by for the Linux
 * kernel by:
 *
 *   Signed-off-by: Richard Fellner <richard.fellner@student.tugraz.at>
 *   Signed-off-by: Moritz Lipp <moritz.lipp@iaik.tugraz.at>
 *   Signed-off-by: Daniel Gruss <daniel.gruss@iaik.tugraz.at>
 *   Signed-off-by: Michael Schwarz <michael.schwarz@iaik.tugraz.at>
 *
 * Major changes to the original code by: Dave Hansen <dave.hansen@intel.com>
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/bug.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#include <asm/kaiser.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/desc.h>

#define KAISER_WALK_ATOMIC  0x1

static pteval_t kaiser_pte_mask __ro_after_init = ~(_PAGE_NX | _PAGE_GLOBAL);

/*
 * At runtime, the only things we map are some things for CPU
 * hotplug, and stacks for new processes.  No two CPUs will ever
 * be populating the same addresses, so we only need to ensure
 * that we protect between two CPUs trying to allocate and
 * populate the same page table page.
 *
 * Only take this lock when doing a set_p[4um]d(), but it is not
 * needed for doing a set_pte().  We assume that only the *owner*
 * of a given allocation will be doing this for _their_
 * allocation.
 *
 * This ensures that once a system has been running for a while
 * and there have been stacks all over and these page tables
 * are fully populated, there will be no further acquisitions of
 * this lock.
 */
static DEFINE_SPINLOCK(shadow_table_allocation_lock);

/*
 * This is only for walking kernel addresses.  We use it to help
 * recreate the "shadow" page tables which are used while we are in
 * userspace.
 *
 * This can be called on any kernel memory addresses and will work
 * with any page sizes and any types: normal linear map memory,
 * vmalloc(), even kmap().
 *
 * Note: this is only used when mapping new *kernel* entries into
 * the user/shadow page tables.  It is never used for userspace
 * addresses.
 *
 * Returns -1 on error.
 */
static inline unsigned long get_pa_from_kernel_map(unsigned long vaddr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	/* We should only be asked to walk kernel addresses */
	if (vaddr < PAGE_OFFSET) {
		WARN_ON_ONCE(1);
		return -1;
	}

	pgd = pgd_offset_k(vaddr);
	/*
	 * We made all the kernel PGDs present in kaiser_init().
	 * We expect them to stay that way.
	 */
	if (pgd_none(*pgd)) {
		WARN_ON_ONCE(1);
		return -1;
	}
	/*
	 * PGDs are either 512GB or 128TB on all x86_64
	 * configurations.  We don't handle these.
	 */
	BUILD_BUG_ON(pgd_large(*pgd) != 0);

	p4d = p4d_offset(pgd, vaddr);
	if (p4d_none(*p4d)) {
		WARN_ON_ONCE(1);
		return -1;
	}

	pud = pud_offset(p4d, vaddr);
	if (pud_none(*pud)) {
		WARN_ON_ONCE(1);
		return -1;
	}

	if (pud_large(*pud))
		return (pud_pfn(*pud) << PAGE_SHIFT) | (vaddr & ~PUD_PAGE_MASK);

	pmd = pmd_offset(pud, vaddr);
	if (pmd_none(*pmd)) {
		WARN_ON_ONCE(1);
		return -1;
	}

	if (pmd_large(*pmd))
		return (pmd_pfn(*pmd) << PAGE_SHIFT) | (vaddr & ~PMD_PAGE_MASK);

	pte = pte_offset_kernel(pmd, vaddr);
	if (pte_none(*pte)) {
		WARN_ON_ONCE(1);
		return -1;
	}

	return (pte_pfn(*pte) << PAGE_SHIFT) | (vaddr & ~PAGE_MASK);
}

/*
 * Walk the shadow copy of the page tables (optionally) trying to
 * allocate page table pages on the way down.  Does not support
 * large pages since the data we are mapping is (generally) not
 * large enough or aligned to 2MB.
 *
 * Note: this is only used when mapping *new* kernel data into the
 * user/shadow page tables.  It is never used for userspace data.
 *
 * Returns a pointer to a PTE on success, or NULL on failure.
 */
static pte_t *kaiser_shadow_pagetable_walk(unsigned long address,
					   unsigned long flags)
{
	pte_t *pte;
	pmd_t *pmd;
	pud_t *pud;
	p4d_t *p4d;
	pgd_t *pgd = kernel_to_shadow_pgdp(pgd_offset_k(address));
	gfp_t gfp = (GFP_KERNEL | __GFP_NOTRACK | __GFP_ZERO);

	if (flags & KAISER_WALK_ATOMIC) {
		gfp &= ~GFP_KERNEL;
		gfp |= __GFP_HIGH | __GFP_ATOMIC;
	}

	if (address < PAGE_OFFSET) {
		WARN_ONCE(1, "attempt to walk user address\n");
		return NULL;
	}

	if (pgd_none(*pgd)) {
		WARN_ONCE(1, "All shadow pgds should have been populated\n");
		return NULL;
	}
	BUILD_BUG_ON(pgd_large(*pgd) != 0);

	p4d = p4d_offset(pgd, address);
	BUILD_BUG_ON(p4d_large(*p4d) != 0);
	if (p4d_none(*p4d)) {
		unsigned long new_pud_page = __get_free_page(gfp);
		if (!new_pud_page)
			return NULL;

		spin_lock(&shadow_table_allocation_lock);
		if (p4d_none(*p4d))
			set_p4d(p4d, __p4d(_KERNPG_TABLE | __pa(new_pud_page)));
		else
			free_page(new_pud_page);
		spin_unlock(&shadow_table_allocation_lock);
	}

	pud = pud_offset(p4d, address);
	/* The shadow page tables do not use large mappings: */
	if (pud_large(*pud)) {
		WARN_ON(1);
		return NULL;
	}
	if (pud_none(*pud)) {
		unsigned long new_pmd_page = __get_free_page(gfp);
		if (!new_pmd_page)
			return NULL;

		spin_lock(&shadow_table_allocation_lock);
		if (pud_none(*pud))
			set_pud(pud, __pud(_KERNPG_TABLE | __pa(new_pmd_page)));
		else
			free_page(new_pmd_page);
		spin_unlock(&shadow_table_allocation_lock);
	}

	pmd = pmd_offset(pud, address);
	/* The shadow page tables do not use large mappings: */
	if (pmd_large(*pmd)) {
		WARN_ON(1);
		return NULL;
	}
	if (pmd_none(*pmd)) {
		unsigned long new_pte_page = __get_free_page(gfp);
		if (!new_pte_page)
			return NULL;

		spin_lock(&shadow_table_allocation_lock);
		if (pmd_none(*pmd))
			set_pmd(pmd, __pmd(_KERNPG_TABLE  | __pa(new_pte_page)));
		else
			free_page(new_pte_page);
		spin_unlock(&shadow_table_allocation_lock);
	}

	pte = pte_offset_kernel(pmd, address);
	if (pte_flags(*pte) & _PAGE_USER) {
		WARN_ONCE(1, "attempt to walk to user pte\n");
		return NULL;
	}
	return pte;
}

/*
 * Given a kernel address, @__start_addr, copy that mapping into
 * the user (shadow) page tables.  This may need to allocate page
 * table pages.
 */
int kaiser_add_user_map(const void *__start_addr, unsigned long size,
			unsigned long flags)
{
	unsigned long start_addr = (unsigned long)__start_addr;
	unsigned long address = start_addr & PAGE_MASK;
	unsigned long end_addr = PAGE_ALIGN(start_addr + size);
	unsigned long target_address;
	pte_t *pte;

	/* Clear not supported bits */
	flags &= kaiser_pte_mask;

	for (; address < end_addr; address += PAGE_SIZE) {
		target_address = get_pa_from_kernel_map(address);
		if (target_address == -1)
			return -EIO;

		pte = kaiser_shadow_pagetable_walk(address, false);
		/*
		 * Errors come from either -ENOMEM for a page
		 * table page, or something screwy that did a
		 * WARN_ON().  Just return -ENOMEM.
		 */
		if (!pte)
			return -ENOMEM;
		if (pte_none(*pte)) {
			set_pte(pte, __pte(flags | target_address));
		} else {
			pte_t tmp;
			/*
			 * Make a fake, temporary PTE that mimics the
			 * one we would have created.
			 */
			set_pte(&tmp, __pte(flags | target_address));
			/*
			 * Warn if the pte that would have been
			 * created is different from the one that
			 * was there previously.  In other words,
			 * we allow the same PTE value to be set,
			 * but not changed.
			 */
			WARN_ON_ONCE(!pte_same(*pte, tmp));
		}
	}
	return 0;
}

int kaiser_add_user_map_ptrs(const void *__start_addr,
			     const void *__end_addr,
			     unsigned long flags)
{
	return kaiser_add_user_map(__start_addr,
				   __end_addr - __start_addr,
				   flags);
}

/*
 * Ensure that the top level of the (shadow) page tables are
 * entirely populated.  This ensures that all processes that get
 * forked have the same entries.  This way, we do not have to
 * ever go set up new entries in older processes.
 *
 * Note: we never free these, so there are no updates to them
 * after this.
 */
static void __init kaiser_init_all_pgds(void)
{
	pgd_t *pgd;
	int i;

	if (__supported_pte_mask & _PAGE_NX)
		kaiser_pte_mask |= _PAGE_NX;
	if (boot_cpu_has(X86_FEATURE_PGE))
		kaiser_pte_mask |= _PAGE_GLOBAL;

	pgd = kernel_to_shadow_pgdp(pgd_offset_k(0UL));
	for (i = PTRS_PER_PGD / 2; i < PTRS_PER_PGD; i++) {
		/*
		 * Each PGD entry moves up PGDIR_SIZE bytes through
		 * the address space, so get the first virtual
		 * address mapped by PGD #i:
		 */
		unsigned long addr = i * PGDIR_SIZE;
#if CONFIG_PGTABLE_LEVELS > 4
		p4d_t *p4d = p4d_alloc_one(&init_mm, addr);
		if (!p4d) {
			WARN_ON(1);
			break;
		}
		set_pgd(pgd + i, __pgd(_KERNPG_TABLE | __pa(p4d)));
#else /* CONFIG_PGTABLE_LEVELS <= 4 */
		pud_t *pud = pud_alloc_one(&init_mm, addr);
		if (!pud) {
			WARN_ON(1);
			break;
		}
		set_pgd(pgd + i, __pgd(_KERNPG_TABLE | __pa(pud)));
#endif /* CONFIG_PGTABLE_LEVELS */
	}
}

/*
 * Page table allocations called by kaiser_add_user_map() can
 * theoretically fail, but are very unlikely to fail in early boot.
 * This would at least output a warning before crashing.
 *
 * Do the checking and warning in a macro to make it more readable and
 * preserve line numbers in the warning message that you would not get
 * with an inline.
 */
#define kaiser_add_user_map_early(start, size, flags) do {	\
	int __ret = kaiser_add_user_map(start, size, flags);	\
	WARN_ON(__ret);						\
} while (0)

#define kaiser_add_user_map_ptrs_early(start, end, flags) do {		\
	int __ret = kaiser_add_user_map_ptrs(start, end, flags);	\
	WARN_ON(__ret);							\
} while (0)

void kaiser_add_mapping_cpu_entry(int cpu)
{
	kaiser_add_user_map_early(get_cpu_gdt_ro(cpu), PAGE_SIZE,
				  __PAGE_KERNEL_RO);

	/* includes the entry stack */
	kaiser_add_user_map_early(&get_cpu_entry_area(cpu)->tss,
				  sizeof(get_cpu_entry_area(cpu)->tss),
				  __PAGE_KERNEL | _PAGE_GLOBAL);

	/* Entry code, so needs to be EXEC */
	kaiser_add_user_map_early(&get_cpu_entry_area(cpu)->entry_trampoline,
				  sizeof(get_cpu_entry_area(cpu)->entry_trampoline),
				  __PAGE_KERNEL_RX | _PAGE_GLOBAL);

	kaiser_add_user_map_early(&get_cpu_entry_area(cpu)->exception_stacks,
				 sizeof(get_cpu_entry_area(cpu)->exception_stacks),
				 __PAGE_KERNEL | _PAGE_GLOBAL);
}

extern char __per_cpu_user_mapped_start[], __per_cpu_user_mapped_end[];
/*
 * If anything in here fails, we will likely die on one of the
 * first kernel->user transitions and init will die.  But, we
 * will have most of the kernel up by then and should be able to
 * get a clean warning out of it.  If we BUG_ON() here, we run
 * the risk of being before we have good console output.
 *
 * When KAISER is enabled, we remove _PAGE_GLOBAL from all of the
 * kernel PTE permissions.  This ensures that the TLB entries for
 * the kernel are not available when in userspace.  However, for
 * the pages that are available to userspace *anyway*, we might as
 * well continue to map them _PAGE_GLOBAL and enjoy the potential
 * performance advantages.
 */
void __init kaiser_init(void)
{
	int cpu;

	kaiser_init_all_pgds();

	for_each_possible_cpu(cpu) {
		void *percpu_vaddr = __per_cpu_user_mapped_start +
				     per_cpu_offset(cpu);
		unsigned long percpu_sz = __per_cpu_user_mapped_end -
					  __per_cpu_user_mapped_start;
		kaiser_add_user_map_early(percpu_vaddr, percpu_sz,
					  __PAGE_KERNEL | _PAGE_GLOBAL);
	}

	kaiser_add_user_map_ptrs_early(__entry_text_start, __entry_text_end,
				       __PAGE_KERNEL_RX | _PAGE_GLOBAL);

	kaiser_add_user_map_ptrs_early(__irqentry_text_start, __irqentry_text_end,
				       __PAGE_KERNEL_RX | _PAGE_GLOBAL);

	/* the fixed map address of the idt_table */
	kaiser_add_user_map_early((void *)idt_descr.address,
				  sizeof(gate_desc) * NR_VECTORS,
				  __PAGE_KERNEL_RO | _PAGE_GLOBAL);

	/*
	 * We delay CPU 0's mappings because these structures are
	 * created before the page allocator is up.  Deferring it
	 * until here lets us use the plain page allocator
	 * unconditionally in the page table code above.
	 *
	 * This is OK because kaiser_init() is called long before
	 * we ever run userspace and need the KAISER mappings.
	 */
	kaiser_add_mapping_cpu_entry(0);
}

int kaiser_add_mapping(unsigned long addr, unsigned long size,
		       unsigned long flags)
{
	return kaiser_add_user_map((const void *)addr, size, flags);
}

void kaiser_remove_mapping(unsigned long start, unsigned long size)
{
	unsigned long addr;

	/* The shadow page tables always use small pages: */
	for (addr = start; addr < start + size; addr += PAGE_SIZE) {
		/*
		 * Do an "atomic" walk in case this got called from an atomic
		 * context.  This should not do any allocations because we
		 * should only be walking things that are known to be mapped.
		 */
		pte_t *pte = kaiser_shadow_pagetable_walk(addr, KAISER_WALK_ATOMIC);

		/*
		 * We are removing a mapping that should
		 * exist.  WARN if it was not there:
		 */
		if (!pte) {
			WARN_ON_ONCE(1);
			continue;
		}

		pte_clear(&init_mm, addr, pte);
	}
	/*
	 * This ensures that the TLB entries used to map this data are
	 * no longer usable on *this* CPU.  We theoretically want to
	 * flush the entries on all CPUs here, but that's too
	 * expensive right now: this is called to unmap process
	 * stacks in the exit() path.
	 *
	 * This can change if we get to the point where this is not
	 * in a remotely hot path, like only called via write_ldt().
	 *
	 * Note: we could probably also just invalidate the individual
	 * addresses to take care of *this* PCID and then do a
	 * tlb_flush_shared_nonglobals() to ensure that all other
	 * PCIDs get flushed before being used again.
	 */
	__native_flush_tlb_global();
}
