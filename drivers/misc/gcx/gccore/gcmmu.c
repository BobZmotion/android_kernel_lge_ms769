/*
 * gcmmu.c
 *
 * Copyright (C) 2010-2011 Vivante Corporation.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "gcmain.h"

#define GCZONE_ALL		(~0U)
#define GCZONE_MAPPING		(1 << 0)
#define GCZONE_CONTEXT		(1 << 1)
#define GCZONE_MASTER		(1 << 2)
#define GCZONE_FIXUP		(1 << 3)
#define GCZONE_FLUSH		(1 << 4)
#define GCZONE_ARENA		(1 << 5)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/pagemap.h>
#include <linux/sched.h>

#include <linux/gcx.h>
#include "gcmmu.h"
#include "gccmdbuf.h"

/*******************************************************************************
 * Internal definitions.
 */

/* Slave table preallocation block; can describe an array of slave tables. */
struct gcmmustlbblock {
	/* Slave table allocation. */
	struct gcpage pages;

	/* Next block of preallocated slave memory. */
	struct gcmmustlbblock *next;
};

static inline struct gcmmu *get_mmu(void)
{
	static struct gcmmu _mmu = {
		.vacarena = LIST_HEAD_INIT(_mmu.vacarena),
	};

	return &_mmu;
}

/*******************************************************************************
 * Arena record management.
 */

#define GCDUMPARENA(text, arena) \
	do { \
		GCPRINT(GCDBGFILTER, GCZONE_ARENA, GC_MOD_PREFIX \
			text ":\n", __func__, __LINE__); \
		GCPRINT(GCDBGFILTER, GCZONE_ARENA, GC_MOD_PREFIX \
			"  number of pages = %u\n", \
			__func__, __LINE__, (arena)->count); \
		GCPRINT(GCDBGFILTER, GCZONE_ARENA, GC_MOD_PREFIX \
			"  from (absolute/mtlb/stlb) = 0x%08X / %u / %u\n", \
			__func__, __LINE__, \
			(arena)->start.absolute, \
			(arena)->start.loc.mtlb, \
			(arena)->start.loc.stlb); \
		GCPRINT(GCDBGFILTER, GCZONE_ARENA, GC_MOD_PREFIX \
			"    to (absolute/mtlb/stlb) = 0x%08X / %u / %u\n", \
			__func__, __LINE__, \
			(arena)->end.absolute, \
			(arena)->end.index.mtlb, \
			(arena)->end.index.stlb); \
	} while (false)

static enum gcerror get_arena(struct gcmmu *mmu, struct gcmmuarena **arena)
{
	enum gcerror gcerror = GCERR_NONE;
	struct gcmmuarena *temp;

	GCPRINT(GCDBGFILTER, GCZONE_ARENA, "++" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	if (list_empty(&mmu->vacarena)) {
		temp = kmalloc(sizeof(struct gcmmuarena), GFP_KERNEL);
		if (temp == NULL) {
			GCPRINT(NULL, 0, GC_MOD_PREFIX
				"arena entry allocation failed.\n",
				__func__, __LINE__);
			gcerror = GCERR_SETGRP(GCERR_OODM,
						GCERR_MMU_ARENA_ALLOC);
			goto exit;
		}
	} else {
		struct list_head *head;
		head = mmu->vacarena.next;
		temp = list_entry(head, struct gcmmuarena, link);
		list_del(head);
	}

	*arena = temp;

exit:
	GCPRINT(GCDBGFILTER, GCZONE_ARENA, "--" GC_MOD_PREFIX
		"gc%s = 0x%08X\n", __func__, __LINE__,
		(gcerror == GCERR_NONE) ? "result" : "error", gcerror);

	return gcerror;
}

static inline bool siblings(struct list_head *head,
				struct list_head *arenahead1,
				struct list_head *arenahead2)
{
	struct gcmmuarena *arena1;
	struct gcmmuarena *arena2;

	if ((arenahead1 == head) || (arenahead2 == head))
		return false;

	arena1 = list_entry(arenahead1, struct gcmmuarena, link);
	arena2 = list_entry(arenahead2, struct gcmmuarena, link);

	return (arena1->end.absolute == arena2->start.absolute) ? true : false;
}

/*******************************************************************************
 * Slave table allocation management.
 */

static enum gcerror allocate_slave(struct gcmmucontext *gcmmucontext,
					union gcmmuloc index)
{
	enum gcerror gcerror;
	struct gcmmustlbblock *block = NULL;
	struct gcmmustlb *slave;
	unsigned int *mtlblogical;
	unsigned int prealloccount;
	unsigned int preallocsize;
	unsigned int preallocentries;
	unsigned int physical;
	unsigned int *logical;
	unsigned int i;

	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, "++" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	/* Allocate a new prealloc block wrapper. */
	block = kmalloc(sizeof(struct gcmmustlbblock), GFP_KERNEL);
	if (block == NULL) {
		GCPRINT(NULL, 0, GC_MOD_PREFIX
			"failed to allocate slave page table wrapper\n",
			__func__, __LINE__);

		gcerror = GCERR_SETGRP(GCERR_OODM,
					GCERR_MMU_STLB_ALLOC);
		goto exit;
	}

	/* Determine the number and the size of tables to allocate. */
	prealloccount = min(GCMMU_STLB_PREALLOC_COUNT,
				GCMMU_MTLB_ENTRY_NUM - index.loc.mtlb);

	preallocsize = prealloccount * GCMMU_STLB_SIZE;
	preallocentries = prealloccount * GCMMU_STLB_ENTRY_NUM;

	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, GC_MOD_PREFIX
		"preallocating %d slave tables.\n",
		__func__, __LINE__, prealloccount);

	/* Allocate slave table pool. */
	gcerror = gc_alloc_cached(&block->pages, preallocsize);
	if (gcerror != GCERR_NONE) {
		GCPRINT(NULL, 0, GC_MOD_PREFIX
			"failed to allocate slave page table\n",
			__func__, __LINE__);

		gcerror = GCERR_SETGRP(gcerror, GCERR_MMU_STLB_ALLOC);
		goto exit;
	}

	/* Add the block to the list. */
	block->next = gcmmucontext->slavealloc;
	gcmmucontext->slavealloc = block;

	/* Get shortcuts to the pointers. */
	physical = block->pages.physical;
	logical = block->pages.logical;

	/* Invalidate all slave entries. */
	for (i = 0; i < preallocentries; i += 1)
		logical[i] = GCMMU_STLB_ENTRY_VACANT;

	/* Init the slaves. */
	slave = &gcmmucontext->slave[index.loc.mtlb];
	mtlblogical = &gcmmucontext->master.logical[index.loc.mtlb];

	for (i = 0; i < prealloccount; i += 1) {
		mtlblogical[i]
			= (physical & GCMMU_MTLB_SLAVE_MASK)
			| GCMMU_MTLB_4K_PAGE
			| GCMMU_MTLB_EXCEPTION
			| GCMMU_MTLB_PRESENT;

		slave[i].physical = physical;
		slave[i].logical = logical;

		physical += GCMMU_STLB_SIZE;
		logical = (unsigned int *)
			((unsigned char *) logical + GCMMU_STLB_SIZE);
	}

	/* Flush CPU cache. */
	gc_flush_region(gcmmucontext->master.physical,
			gcmmucontext->master.logical,
			index.loc.mtlb * sizeof(unsigned int),
			prealloccount * sizeof(unsigned int));

	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, "--" GC_MOD_PREFIX,
		__func__, __LINE__);

	return GCERR_NONE;

exit:
	if (block != NULL)
		kfree(block);

	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, "--" GC_MOD_PREFIX
		"gcerror = 0x%08X\n", __func__, __LINE__, gcerror);

	return gcerror;
}

/*******************************************************************************
 * Physical page array generation.
 */

static enum gcerror virt2phys(unsigned int logical, pte_t *physical)
{
	pgd_t *pgd;	/* Page Global Directory (PGD). */
	pmd_t *pmd;	/* Page Middle Directory (PMD). */
	pte_t *pte;	/* Page Table Entry (PTE). */

	/* Get the pointer to the entry in PGD for the address. */
	pgd = pgd_offset(current->mm, logical);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return GCERR_MMU_PAGE_BAD;

	/* Get the pointer to the entry in PMD for the address. */
	pmd = pmd_offset(pgd, logical);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return GCERR_MMU_PAGE_BAD;

	/* Get the pointer to the entry in PTE for the address. */
	pte = pte_offset_map(pmd, logical);
	if ((pte == NULL) || !pte_present(*pte))
		return GCERR_MMU_PAGE_BAD;

	*physical = (*pte & PAGE_MASK) | (logical & ~PAGE_MASK);
	return GCERR_NONE;
}

#if !defined(PFN_DOWN)
#	define PFN_DOWN(x) \
		((x) >> PAGE_SHIFT)
#endif

#if !defined(phys_to_pfn)
#	define phys_to_pfn(phys) \
		(PFN_DOWN(phys))
#endif

#if !defined(phys_to_page)
#	define phys_to_page(paddr) \
		(pfn_to_page(phys_to_pfn(paddr)))
#endif

static enum gcerror get_physical_pages(struct gcmmuphysmem *mem,
					pte_t *parray,
					struct gcmmuarena *arena)
{
	enum gcerror gcerror = GCERR_NONE;
	struct vm_area_struct *vma;
	struct page **pages = NULL;
	unsigned int base, write;
	int i, count = 0;

	/* Reset page descriptor array. */
	arena->pages = NULL;

	/* Get base address shortcut. */
	base = mem->base;

	/* Store the logical pointer. */
	arena->logical = (void *) base;

	/*
	 * Important Note: base is mapped from user application process
	 * to current process - it must lie completely within the current
	 * virtual memory address space in order to be of use to us here.
	 */

	vma = find_vma(current->mm, base + (mem->count << PAGE_SHIFT) - 1);
	if ((vma == NULL) || (base < vma->vm_start)) {
		gcerror = GCERR_MMU_BUFFER_BAD;
		goto exit;
	}

	/* Allocate page descriptor array. */
	pages = kmalloc(mem->count * sizeof(struct page *), GFP_KERNEL);
	if (pages == NULL) {
		gcerror = GCERR_SETGRP(GCERR_OODM, GCERR_MMU_DESC_ALLOC);
		goto exit;
	}

	/* Query page descriptors. */
	write = ((vma->vm_flags & (VM_WRITE | VM_MAYWRITE)) != 0) ? 1 : 0;
	count = get_user_pages(current, current->mm, base, mem->count,
				write, 1, pages, NULL);

	if (count < 0) {
		/* Kernel allocated buffer. */
		for (i = 0; i < mem->count; i += 1) {
			gcerror = virt2phys(base, &parray[i]);
			if (gcerror != GCERR_NONE)
				goto exit;

			base += mem->pagesize;
		}
	} else if (count == mem->count) {
		/* User allocated buffer. */
		for (i = 0; i < mem->count; i += 1) {
			parray[i] = page_to_phys(pages[i]);
			if (phys_to_page(parray[i]) != pages[i]) {
				gcerror = GCERR_MMU_PAGE_BAD;
				goto exit;
			}
		}

		/* Set page descriptor array. */
		arena->pages = pages;
	} else {
		gcerror = GCERR_MMU_BUFFER_BAD;
		goto exit;
	}

exit:
	if (arena->pages == NULL) {
		for (i = 0; i < count; i += 1)
			page_cache_release(pages[i]);

		if (pages != NULL)
			kfree(pages);
	}

	return gcerror;
}

static void release_physical_pages(struct gcmmuarena *arena)
{
	unsigned int i;

	if (arena->pages != NULL) {
		for (i = 0; i < arena->count; i += 1)
			page_cache_release(arena->pages[i]);

		kfree(arena->pages);
		arena->pages = NULL;
	}
}

/*******************************************************************************
 * MMU management API.
 */

enum gcerror gcmmu_create_context(struct gcmmucontext *gcmmucontext)
{
	enum gcerror gcerror;
	struct gcmmu *mmu = get_mmu();
	struct gcmmuarena *arena = NULL;
	unsigned int *logical;
	unsigned int safecount;
	int i;

	GCPRINT(GCDBGFILTER, GCZONE_CONTEXT, "++" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	if (gcmmucontext == NULL) {
		gcerror = GCERR_MMU_CTXT_BAD;
		goto exit;
	}

	/* Reset the context. */
	memset(gcmmucontext, 0, sizeof(struct gcmmucontext));

	/* Initialize arena lists. */
	INIT_LIST_HEAD(&gcmmucontext->vacant);
	INIT_LIST_HEAD(&gcmmucontext->allocated);

	/* Allocate MTLB table. */
	gcerror = gc_alloc_cached(&gcmmucontext->master, GCMMU_MTLB_SIZE);
	if (gcerror != GCERR_NONE) {
		gcerror = GCERR_SETGRP(gcerror, GCERR_MMU_MTLB_ALLOC);
		goto exit;
	}

	/* Invalidate MTLB entries. */
	logical = gcmmucontext->master.logical;
	for (i = 0; i < GCMMU_MTLB_ENTRY_NUM; i += 1)
		logical[i] = GCMMU_MTLB_ENTRY_VACANT;

	/* Set MMU table mode. */
	gcmmucontext->mmuconfig.reg.master_mask
		= GCREG_MMU_CONFIGURATION_MASK_MODE_ENABLED;
	gcmmucontext->mmuconfig.reg.master = GCMMU_MTLB_MODE;

	/* Set the table address. */
	gcmmucontext->mmuconfig.reg.address_mask
		= GCREG_MMU_CONFIGURATION_MASK_ADDRESS_ENABLED;
	gcmmucontext->mmuconfig.reg.address
		= GETFIELD(gcmmucontext->master.physical,
				GCREG_MMU_CONFIGURATION, ADDRESS);

	/* Allocate the first vacant arena. */
	gcerror = get_arena(mmu, &arena);
	if (gcerror != GCERR_NONE)
		goto exit;

	/* Entire range is currently vacant. */
	arena->start.absolute = 0;
	arena->end.absolute =
	arena->count = GCMMU_MTLB_ENTRY_NUM * GCMMU_STLB_ENTRY_NUM;
	list_add(&arena->link, &gcmmucontext->vacant);
	GCDUMPARENA("initial vacant arena", arena);

	/* Allocate the safe zone. */
	if (mmu->safezone.size == 0) {
		gcerror = gc_alloc_cached(&mmu->safezone,
						GCMMU_SAFE_ZONE_SIZE);
		if (gcerror != GCERR_NONE) {
			gcerror = GCERR_SETGRP(gcerror,
						GCERR_MMU_SAFE_ALLOC);
			goto exit;
		}

		/* Initialize safe zone to a value. */
		safecount = GCMMU_SAFE_ZONE_SIZE / sizeof(unsigned int);
		for (i = 0; i < safecount; i += 1)
			mmu->safezone.logical[i] = 0xDEADC0DE;

		gc_flush_region(mmu->safezone.physical, mmu->safezone.logical,
				0, safecount * sizeof(unsigned int));
	}

	/* Reference MMU. */
	mmu->refcount += 1;
	gcmmucontext->mmu = mmu;

	GCPRINT(GCDBGFILTER, GCZONE_CONTEXT, "--" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	return GCERR_NONE;

exit:
	gcmmu_destroy_context(gcmmucontext);

	GCPRINT(GCDBGFILTER, GCZONE_CONTEXT, "--" GC_MOD_PREFIX
		"gcerror = 0x%08X\n", __func__, __LINE__, gcerror);

	return gcerror;
}

enum gcerror gcmmu_destroy_context(struct gcmmucontext *gcmmucontext)
{
	enum gcerror gcerror;
	struct gcmmustlbblock *nextblock;
	struct gcmmu *mmu;

	GCPRINT(GCDBGFILTER, GCZONE_CONTEXT, "++" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	if ((gcmmucontext == NULL) || (gcmmucontext->mmu == NULL)) {
		gcerror = GCERR_MMU_CTXT_BAD;
		goto exit;
	}

	/* Free slave tables. */
	while (gcmmucontext->slavealloc != NULL) {
		gc_free_cached(&gcmmucontext->slavealloc->pages);
		nextblock = gcmmucontext->slavealloc->next;
		kfree(gcmmucontext->slavealloc);
		gcmmucontext->slavealloc = nextblock;
	}

	/* Free the master table. */
	gc_free_cached(&gcmmucontext->master);

	/* Free arenas. */
	mmu = gcmmucontext->mmu;
	list_splice_init(&gcmmucontext->vacant, &mmu->vacarena);
	list_splice_init(&gcmmucontext->allocated, &mmu->vacarena);

	/* Dereference. */
	mmu->refcount -= 1;
	gcmmucontext->mmu = NULL;

	GCPRINT(GCDBGFILTER, GCZONE_CONTEXT, "--" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	return GCERR_NONE;

exit:
	GCPRINT(GCDBGFILTER, GCZONE_CONTEXT, "--" GC_MOD_PREFIX
		"gcerror = 0x%08X\n", __func__, __LINE__, gcerror);

	return gcerror;
}

enum gcerror gcmmu_set_master(struct gcmmucontext *gcmmucontext)
{
	enum gcerror gcerror;
	struct gcmommumaster *gcmommumaster;
	struct gcmommuinit *gcmommuinit;
	unsigned int size, status, enabled;
	struct gcmmu *mmu = get_mmu();

	GCPRINT(GCDBGFILTER, GCZONE_MASTER, "++" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	if ((gcmmucontext == NULL) || (gcmmucontext->mmu == NULL)) {
		gcerror = GCERR_MMU_CTXT_BAD;
		goto exit;
	}

	/* Read the MMU status. */
	status = gc_read_reg(GCREG_MMU_CONTROL_Address);
	enabled = GETFIELD(status, GCREG_MMU_CONTROL, ENABLE);

	/* Is MMU enabled? */
	if (enabled) {
		/* Allocate command buffer space. */
		gcerror = cmdbuf_alloc(sizeof(struct gcmommumaster),
					(void **) &gcmommumaster, NULL);
		if (gcerror != GCERR_NONE) {
			gcerror = GCERR_SETGRP(gcerror, GCERR_MMU_MTLB_SET);
			goto exit;
		}

		/* Program master table address. */
		gcmommumaster->master_ldst = gcmommumaster_master_ldst;
		gcmommumaster->master = gcmmucontext->mmuconfig.raw;
	} else {
		GCPRINT(GCDBGFILTER, GCZONE_MASTER, GC_MOD_PREFIX
			"enabling MMU.\n", __func__, __LINE__);

		/* MMU disabled, force physical mode. */
		cmdbuf_physical(true);

		/* Allocate command buffer space. */
		size = sizeof(struct gcmommuinit) + cmdbuf_flush(NULL);
		gcerror = cmdbuf_alloc(size, (void **) &gcmommuinit, NULL);
		if (gcerror != GCERR_NONE) {
			gcerror = GCERR_SETGRP(gcerror, GCERR_MMU_INIT);
			goto exit;
		}

		/* Program the safe zone and the master table address. */
		gcmommuinit->safe_ldst = gcmommuinit_safe_ldst;
		gcmommuinit->safe = mmu->safezone.physical;
		gcmommuinit->mtlb = gcmmucontext->mmuconfig.raw;

		/* Execute the buffer. */
		cmdbuf_flush(gcmommuinit + 1);

		/* Resume normal mode. */
		cmdbuf_physical(false);

		/*
		* Enable MMU. For security reasons, once it is enabled,
		* the only way to disable is to reset the system.
		*/
		gc_write_reg(
			GCREG_MMU_CONTROL_Address,
			SETFIELDVAL(0, GCREG_MMU_CONTROL, ENABLE, ENABLE));
	}

exit:
	GCPRINT(GCDBGFILTER, GCZONE_MASTER, "--" GC_MOD_PREFIX
		"gc%s = 0x%08X\n", __func__, __LINE__,
		(gcerror == GCERR_NONE) ? "result" : "error", gcerror);

	return gcerror;
}

enum gcerror gcmmu_map(struct gcmmucontext *gcmmucontext,
			struct gcmmuphysmem *mem,
			struct gcmmuarena **mapped)
{
	enum gcerror gcerror = GCERR_NONE;
	struct list_head *arenahead;
	struct gcmmuarena *vacant = NULL, *split;
	struct gcmmustlb *slave;
	unsigned int *stlblogical;
	union gcmmuloc index;
	unsigned int i, allocated, count;
	pte_t *parray_alloc = NULL;
	pte_t *parray;

	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, "++" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	if ((gcmmucontext == NULL) || (gcmmucontext->mmu == NULL)) {
		gcerror = GCERR_MMU_CTXT_BAD;
		goto exit;
	}

	if ((mem == NULL) || (mem->count <= 0) || (mapped == NULL) ||
		((mem->pagesize != 0) && (mem->pagesize != GCMMU_PAGE_SIZE))) {
		gcerror = GCERR_MMU_ARG;
		goto exit;
	}

	/*
	 * Find available sufficient arena.
	 */

	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, GC_MOD_PREFIX
		"mapping (%d) pages\n",
		__func__, __LINE__, mem->count);

	list_for_each(arenahead, &gcmmucontext->vacant) {
		vacant = list_entry(arenahead, struct gcmmuarena, link);
		if (vacant->count >= mem->count)
			break;
	}

	if (arenahead == &gcmmucontext->vacant) {
		gcerror = GCERR_MMU_OOM;
		goto exit;
	}

	GCDUMPARENA("allocating from arena", vacant);

	/*
	 * If page array isn't provided, create it here.
	 */

	/* Reset page array. */
	vacant->pages = NULL;

	/* No page array given? */
	if (mem->pages == NULL) {
		/* Allocate physical address array. */
		parray_alloc = kmalloc(mem->count * sizeof(pte_t *),
					GFP_KERNEL);
		if (parray_alloc == NULL) {
			gcerror = GCERR_SETGRP(GCERR_OODM,
						GCERR_MMU_PHYS_ALLOC);
			goto exit;
		}

		/* Fetch page addresses. */
		gcerror = get_physical_pages(mem, parray_alloc, vacant);
		if (gcerror != GCERR_NONE)
			goto exit;

		parray = parray_alloc;

		GCPRINT(GCDBGFILTER, GCZONE_MAPPING, GC_MOD_PREFIX
			"physical page array allocated (0x%08X)\n",
			__func__, __LINE__, (unsigned int) parray);
	} else {
		parray = mem->pages;

		GCPRINT(GCDBGFILTER, GCZONE_MAPPING, GC_MOD_PREFIX
			"physical page array provided (0x%08X)\n",
			__func__, __LINE__, (unsigned int) parray);
	}

	/*
	 * Create the mapping.
	 */

	index.absolute = vacant->start.absolute;
	slave = &gcmmucontext->slave[index.loc.mtlb];
	count = mem->count;

	while (count > 0) {
		/* Allocate slaves if not yet allocated. */
		if (slave->logical == NULL) {
			gcerror = allocate_slave(gcmmucontext, index);
			if (gcerror != GCERR_NONE)
				goto exit;
		}

		/* Determine the number of entries allocated. */
		allocated = GCMMU_STLB_ENTRY_NUM - index.loc.stlb;
		if (allocated > count)
			allocated = count;

		/* Initialize slave entries. */
		stlblogical = &slave->logical[index.loc.stlb];
		for (i = 0; i < allocated; i += 1)
			*stlblogical++
				= (*parray++ & GCMMU_STLB_ADDRESS_MASK)
				| GCMMU_STLB_PRESENT
				| GCMMU_STLB_EXCEPTION
				| GCMMU_STLB_WRITEABLE;

		/* Flush CPU cache. */
		gc_flush_region(slave->physical, slave->logical,
				index.loc.stlb * sizeof(unsigned int),
				allocated * sizeof(unsigned int));

		GCPRINT(GCDBGFILTER, GCZONE_MAPPING, GC_MOD_PREFIX
			"allocated %d pages at %d.%d\n",
			__func__, __LINE__,
			allocated, index.loc.mtlb, index.loc.stlb);

		/* Advance. */
		slave += 1;
		index.absolute += allocated;
		count -= allocated;
	}

	/*
	 * Claim arena.
	 */

	/* Split the arena. */
	if (vacant->count != mem->count) {
		GCPRINT(GCDBGFILTER, GCZONE_MAPPING, GC_MOD_PREFIX
			"splitting vacant arena\n",
			__func__, __LINE__);

		gcerror = get_arena(gcmmucontext->mmu, &split);
		if (gcerror != GCERR_NONE)
			goto exit;

		split->start.absolute = index.absolute;
		split->end.absolute = vacant->end.absolute;
		split->count = vacant->count - mem->count;
		list_add(&split->link, &vacant->link);

		vacant->end.absolute = index.absolute;
		vacant->count = mem->count;
	}

	GCDUMPARENA("allocated arena", vacant);

	/* Move the vacant arena to the list of allocated arenas. */
	list_move(&vacant->link, &gcmmucontext->allocated);

	/* Set page size. */
	mem->pagesize = GCMMU_PAGE_SIZE;

	/* Determine the virtual address. */
	vacant->address
		= ((vacant->start.loc.mtlb << GCMMU_MTLB_SHIFT)
					    & GCMMU_MTLB_MASK)
		| ((vacant->start.loc.stlb << GCMMU_STLB_SHIFT)
					    & GCMMU_STLB_MASK)
		| (mem->offset & GCMMU_OFFSET_MASK);

	/* Determine the size of the area. */
	vacant->size = mem->count * GCMMU_PAGE_SIZE - mem->offset;

	/* Set the result. */
	*mapped = vacant;

	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, GC_MOD_PREFIX
		"mapped %d bytes at 0x%08X\n",
		__func__, __LINE__,
		vacant->size, vacant->address);

exit:
	if (parray_alloc != NULL) {
		kfree(parray_alloc);

		if (gcerror != GCERR_NONE)
			release_physical_pages(vacant);
	}

	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, "--" GC_MOD_PREFIX
		"gc%s = 0x%08X\n", __func__, __LINE__,
		(gcerror == GCERR_NONE) ? "result" : "error", gcerror);

	return gcerror;
}

enum gcerror gcmmu_unmap(struct gcmmucontext *gcmmucontext,
				struct gcmmuarena *mapped)
{
	enum gcerror gcerror = GCERR_NONE;
	struct list_head *allochead, *prevhead, *nexthead;
	struct gcmmuarena *allocated, *prevvacant, *nextvacant = NULL;
	struct gcmmustlb *slave;
	unsigned int *stlblogical;
	union gcmmuloc index;
	unsigned int i, freed, count;
	struct gcmmu *mmu = get_mmu();

	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, "++" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	if ((gcmmucontext == NULL) || (gcmmucontext->mmu == NULL)) {
		gcerror = GCERR_MMU_CTXT_BAD;
		goto exit;
	}

	/*
	 * Find the arena.
	 */

	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, GC_MOD_PREFIX
		"unmapping arena 0x%08X\n",
		__func__, __LINE__, (unsigned int) mapped);

	list_for_each(allochead, &gcmmucontext->allocated) {
		allocated = list_entry(allochead, struct gcmmuarena, link);
		if (allocated == mapped)
			break;
	}

	if (allochead == &gcmmucontext->allocated) {
		gcerror = GCERR_MMU_ARG;
		goto exit;
	}

	/*
	 * Free slave tables.
	 */

	index.absolute = allocated->start.absolute;
	slave = &gcmmucontext->slave[index.loc.mtlb];
	count = allocated->count;

	while (count > 0) {
		/* Determine the number of entries freed. */
		freed = GCMMU_STLB_ENTRY_NUM - index.loc.stlb;
		if (freed > count)
			freed = count;

		/* Free slave entries. */
		stlblogical = &slave->logical[index.loc.stlb];
		for (i = 0; i < freed; i += 1)
			*stlblogical++ = GCMMU_STLB_ENTRY_VACANT;

		/* Flush CPU cache. */
		gc_flush_region(slave->physical, slave->logical,
				index.loc.stlb * sizeof(unsigned int),
				freed * sizeof(unsigned int));

		GCPRINT(GCDBGFILTER, GCZONE_MAPPING, GC_MOD_PREFIX
			"freed %d pages at %d.%d\n",
			__func__, __LINE__,
			freed, index.loc.mtlb, index.loc.stlb);

		/* Advance. */
		slave += 1;
		index.absolute += freed;
		count -= freed;
	}

	/*
	 * Delete page cache for the arena.
	 */

	release_physical_pages(allocated);

	/*
	 * Find point of insertion and free the arena.
	 */

	list_for_each(nexthead, &gcmmucontext->vacant) {
		nextvacant = list_entry(nexthead, struct gcmmuarena, link);
		if (nextvacant->start.absolute >= allocated->end.absolute)
			break;
	}

	/* Get the previous vacant entry. */
	prevhead = nexthead->prev;

	/* Merge the area back into vacant list. */
	if (siblings(&gcmmucontext->vacant, prevhead, allochead)) {
		if (siblings(&gcmmucontext->vacant, allochead, nexthead)) {
			prevvacant = list_entry(prevhead, struct gcmmuarena,
						link);

			/* Merge all three arenas. */
			prevvacant->count += allocated->count;
			prevvacant->count += nextvacant->count;
			prevvacant->end.absolute = nextvacant->end.absolute;

			/* Free the merged arenas. */
			list_move(allochead, &mmu->vacarena);
			list_move(nexthead, &mmu->vacarena);
		} else {
			prevvacant = list_entry(prevhead, struct gcmmuarena,
						link);

			/* Merge with the previous. */
			prevvacant->count += allocated->count;
			prevvacant->end.absolute = allocated->end.absolute;

			/* Free the merged arena. */
			list_move(allochead, &mmu->vacarena);
		}
	} else if (siblings(&gcmmucontext->vacant, allochead, nexthead)) {
		/* Merge with the next arena. */
		nextvacant->start.absolute = allocated->start.absolute;
		nextvacant->count += allocated->count;

		/* Free the merged arena. */
		list_move(allochead, &mmu->vacarena);
	} else {

		GCPRINT(GCDBGFILTER, GCZONE_MAPPING, GC_MOD_PREFIX
			"cannot merge, inserting in between:\n",
			__func__, __LINE__);

		/* Neighbor vacant arenas are not siblings, can't merge. */
		list_move(allochead, prevhead);
	}

exit:
	GCPRINT(GCDBGFILTER, GCZONE_MAPPING, "--" GC_MOD_PREFIX
		"gc%s = 0x%08X\n", __func__, __LINE__,
		(gcerror == GCERR_NONE) ? "result" : "error", gcerror);

	return gcerror;
}

int gcmmu_flush(void *logical, unsigned int address, unsigned int size)
{
	static const int flushSize = sizeof(struct gcmommuflush);
	struct gcmommuflush *gcmommuflush;
	unsigned int count;

	GCPRINT(GCDBGFILTER, GCZONE_FLUSH, "++" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	if (logical != NULL) {
		GCPRINT(GCDBGFILTER, GCZONE_FLUSH, GC_MOD_PREFIX
			"address = 0x%08X\n",
			__func__, __LINE__, address);

		GCPRINT(GCDBGFILTER, GCZONE_FLUSH, GC_MOD_PREFIX
			"size = %d\n",
			__func__, __LINE__, size);

		/* Compute the buffer count. */
		count = (size - flushSize + 7) >> 3;

		gcmommuflush = (struct gcmommuflush *) logical;

		/* Flush 2D PE cache. */
		gcmommuflush->peflush.flush_ldst = gcmoflush_flush_ldst;
		gcmommuflush->peflush.flush.reg = gcregflush_pe2D;

		/* Arm the FE-PE semaphore. */
		gcmommuflush->peflushsema.sema_ldst = gcmosema_sema_ldst;
		gcmommuflush->peflushsema.sema.reg  = gcregsema_fe_pe;

		/* Stall FE until PE is done flushing. */
		gcmommuflush->peflushstall.cmd.fld = gcfldstall;
		gcmommuflush->peflushstall.arg.fld = gcfldstall_fe_pe;

		/* LINK to the next slot to flush FE FIFO. */
		gcmommuflush->feflush.cmd.fld = gcfldlink4;
		gcmommuflush->feflush.address
			= address
			+ offsetof(struct gcmommuflush, mmuflush_ldst);

		/* Flush MMU cache. */
		gcmommuflush->mmuflush_ldst = gcmommuflush_mmuflush_ldst;
		gcmommuflush->mmuflush.reg = gcregmmu_flush;

		/* Arm the FE-PE semaphore. */
		gcmommuflush->mmuflushsema.sema_ldst = gcmosema_sema_ldst;
		gcmommuflush->mmuflushsema.sema.reg  = gcregsema_fe_pe;

		/* Stall FE until PE is done flushing. */
		gcmommuflush->mmuflushstall.cmd.fld = gcfldstall;
		gcmommuflush->mmuflushstall.arg.fld = gcfldstall_fe_pe;

		/* LINK to the next slot to flush FE FIFO. */
		gcmommuflush->link.cmd.fld.opcode
			= GCREG_COMMAND_LINK_COMMAND_OPCODE_LINK;
		gcmommuflush->link.cmd.fld.count = count;
		gcmommuflush->link.address = address + flushSize;
	}

	GCPRINT(GCDBGFILTER, GCZONE_FLUSH, "--" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	/* Return the size in bytes required for the flush. */
	return flushSize;
}

enum gcerror gcmmu_fixup(struct gcfixup *fixup, unsigned int *data)
{
	enum gcerror gcerror = GCERR_NONE;
	struct gcfixupentry *table;
	struct gcmmuarena *arena;
	unsigned int dataoffset;
	unsigned int surfoffset;
	unsigned int i;

	GCPRINT(GCDBGFILTER, GCZONE_FIXUP, "++" GC_MOD_PREFIX
		"\n", __func__, __LINE__);

	/* Process fixups. */
	while (fixup != NULL) {
		GCPRINT(GCDBGFILTER, GCZONE_FIXUP, GC_MOD_PREFIX
			"processing %d fixup(s) @ 0x%08X\n",
			__func__, __LINE__, fixup->count, (unsigned int) fixup);

		/* Apply fixups. */
		table = fixup->fixup;
		for (i = 0; i < fixup->count; i += 1) {
			GCPRINT(GCDBGFILTER, GCZONE_FIXUP, GC_MOD_PREFIX
				"[%02d] buffer offset = 0x%08X, "
				"surface offset = 0x%08X\n",
				__func__, __LINE__, i,
				table->dataoffset * 4,
				table->surfoffset);

			dataoffset = table->dataoffset;
			arena = (struct gcmmuarena *) data[dataoffset];

			GCPRINT(GCDBGFILTER, GCZONE_FIXUP, GC_MOD_PREFIX
				"arena = 0x%08X\n",
				__func__, __LINE__,  (unsigned int) arena);
			GCPRINT(GCDBGFILTER, GCZONE_FIXUP, GC_MOD_PREFIX
				"arena phys = 0x%08X\n",
				__func__, __LINE__, arena->address);
			GCPRINT(GCDBGFILTER, GCZONE_FIXUP, GC_MOD_PREFIX
				"arena size = %d\n",
				__func__, __LINE__, arena->size);

			surfoffset = table->surfoffset;

#if 0
			if (surfoffset > arena->size) {
				gcerror = GCERR_MMU_OFFSET;
				goto exit;
			}
#endif

			data[dataoffset] = arena->address + surfoffset;

			GCPRINT(GCDBGFILTER, GCZONE_FIXUP, GC_MOD_PREFIX
				"surface address = 0x%08X\n",
				__func__, __LINE__, data[dataoffset]);

			table += 1;
		}

		/* Get the next fixup. */
		fixup = fixup->next;
	}

	GCPRINT(GCDBGFILTER, GCZONE_FIXUP, "--" GC_MOD_PREFIX
		"gc%s = 0x%08X\n", __func__, __LINE__,
		(gcerror == GCERR_NONE) ? "result" : "error", gcerror);

	return gcerror;
}
