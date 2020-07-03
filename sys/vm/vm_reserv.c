/*-
 * Copyright (c) 2002-2006 Rice University
 * Copyright (c) 2007-2011 Alan L. Cox <alc@cs.rice.edu>
 * All rights reserved.
 *
 * This software was developed for the FreeBSD Project by Alan L. Cox,
 * Olivier Crameri, Peter Druschel, Sitaram Iyer, and Juan Navarro.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *	Superpage reservation management module
 *
 * Any external functions defined by this module are only to be used by the
 * virtual memory system.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_vm.h"

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/vmmeter.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_phys.h>
#include <vm/vm_radix.h>
#include <vm/vm_reserv.h>

/* reserv idle daemon which zeros pages inside reservations */
#include <opt_sched.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/kthread.h>
#include <sys/unistd.h>



/*
 * The reservation system supports the speculative allocation of large physical
 * pages ("superpages").  Speculative allocation enables the fully automatic
 * utilization of superpages by the virtual memory system.  In other words, no
 * programmatic directives are required to use superpages.
 */

#if VM_NRESERVLEVEL > 0

/*
 * The number of small pages that are contained in a level 0 reservation
 */
#define	VM_LEVEL_0_NPAGES	(1 << VM_LEVEL_0_ORDER)

/*
 * The number of bits by which a physical address is shifted to obtain the
 * reservation number
 */
#define	VM_LEVEL_0_SHIFT	(VM_LEVEL_0_ORDER + PAGE_SHIFT)

/*
 * The size of a level 0 reservation in bytes
 */
#define	VM_LEVEL_0_SIZE		(1 << VM_LEVEL_0_SHIFT)

/*
 * Computes the index of the small page underlying the given (object, pindex)
 * within the reservation's array of small pages.
 */
#define	VM_RESERV_INDEX(object, pindex)	\
    (((object)->pg_color + (pindex)) & (VM_LEVEL_0_NPAGES - 1))

/*
 * The size of a population map entry
 */
typedef	u_long		popmap_t;

/*
 * The number of bits in a population map entry
 */
#define	NBPOPMAP	(NBBY * sizeof(popmap_t))

/*
 * The number of population map entries in a reservation
 */
#define	NPOPMAP		howmany(VM_LEVEL_0_NPAGES, NBPOPMAP)

/*
 * Clear a bit in the population map.
 */
static __inline void
popmap_clear(popmap_t popmap[], int i)
{

	popmap[i / NBPOPMAP] &= ~(1UL << (i % NBPOPMAP));
}

/*
 * Set a bit in the population map.
 */
static __inline void
popmap_set(popmap_t popmap[], int i)
{

	popmap[i / NBPOPMAP] |= 1UL << (i % NBPOPMAP);
}

/*
 * Is a bit in the population map clear?
 */
static __inline boolean_t
popmap_is_clear(popmap_t popmap[], int i)
{

	return ((popmap[i / NBPOPMAP] & (1UL << (i % NBPOPMAP))) == 0);
}

/*
 * Is a bit in the population map set?
 */
static __inline boolean_t
popmap_is_set(popmap_t popmap[], int i)
{

	return ((popmap[i / NBPOPMAP] & (1UL << (i % NBPOPMAP))) != 0);
}

/*
 * encode inpartpopq, need-to-migrate and marker status in inpartpopq
 * TODO:
 *	1. need an extra thread to migrate detected reservations and use
 *  clear_migrate(x)
 */
#define RV_INPARTPOPQ		0x01
#define RV_TRANSFERRED		0x02
#define RV_NEEDMIGRATE 		0x04
#define RV_MARKER			0x08
#define RV_BADBOY			0x10
#define clear_all(x)		(x = 0)
#define is_inpartpopq(x)	(x & RV_INPARTPOPQ)
#define clear_inpartpopq(x)	(x &= ~RV_INPARTPOPQ)
#define set_inpartpopq(x)   (x |= RV_INPARTPOPQ)
#define settime(rv)			(rv->timestamp = ticks)
#define need_migrate(x)		(x & RV_NEEDMIGRATE)
#define clear_migrate(x)	(x &= ~RV_NEEDMIGRATE)
#define set_migrate(x)		{x |= RV_NEEDMIGRATE;numofdeadbeef ++;}
/*
 * The reservation structure
 *
 * A reservation structure is constructed whenever a large physical page is
 * speculatively allocated to an object.  The reservation provides the small
 * physical pages for the range [pindex, pindex + VM_LEVEL_0_NPAGES) of offsets
 * within that object.  The reservation's "popcnt" tracks the number of these
 * small physical pages that are in use at any given time.  When and if the
 * reservation is not fully utilized, it appears in the queue of partially
 * populated reservations.  The reservation always appears on the containing
 * object's list of reservations.
 *
 * A partially populated reservation can be broken and reclaimed at any time.
 */
struct vm_reserv {
	TAILQ_ENTRY(vm_reserv) partpopq;
	LIST_ENTRY(vm_reserv) objq;
	vm_object_t	object;			/* containing object */
	vm_pindex_t	pindex;			/* offset within object */
	vm_page_t	pages;			/* first page of a superpage */
	int		popcnt;			/* # of pages in use */
	int 	timestamp; 		/* timestamp last populated/depopulated */
	char		inpartpopq;
	popmap_t	popmap[NPOPMAP];	/* bit vector of used pages */
};

/* initialize the marker to help asyncpromo scan partpopq */
struct vm_reserv async_marker, evict_marker, compact_marker;

/* The marker initializer does not assign the memory */
static void vm_reserv_init_marker(vm_reserv_t rv)
{
	bzero(rv, sizeof(*rv));
	rv->object = NULL;
	rv->inpartpopq = RV_INPARTPOPQ | RV_MARKER;
	for(int i = 0; i < VM_LEVEL_0_NPAGES; i ++)
		popmap_clear(rv->popmap, i);
}
/*
 * [asyncpromo]
 * TAILQ headname for struct vm_reserv
 */
TAILQ_HEAD(rvlist, vm_reserv);

/*
 * The reservation array
 *
 * This array is analoguous in function to vm_page_array.  It differs in the
 * respect that it may contain a greater number of useful reservation
 * structures than there are (physical) superpages.  These "invalid"
 * reservation structures exist to trade-off space for time in the
 * implementation of vm_reserv_from_page().  Invalid reservation structures are
 * distinguishable from "valid" reservation structures by inspecting the
 * reservation's "pages" field.  Invalid reservation structures have a NULL
 * "pages" field.
 *
 * vm_reserv_from_page() maps a small (physical) page to an element of this
 * array by computing a physical reservation number from the page's physical
 * address.  The physical reservation number is used as the array index.
 *
 * An "active" reservation is a valid reservation structure that has a non-NULL
 * "object" field and a non-zero "popcnt" field.  In other words, every active
 * reservation belongs to a particular object.  Moreover, every active
 * reservation has an entry in the containing object's list of reservations.  
 */
static vm_reserv_t vm_reserv_array;

/*
 * The partially populated reservation queue
 *
 * This queue enables the fast recovery of an unused free small page from a
 * partially populated reservation.  The reservation at the head of this queue
 * is the least recently changed, partially populated reservation.
 *
 * Access to this queue is synchronized by the free page queue lock.
 */
static TAILQ_HEAD(, vm_reserv) vm_rvq_partpop =
			    TAILQ_HEAD_INITIALIZER(vm_rvq_partpop);

static SYSCTL_NODE(_vm, OID_AUTO, reserv, CTLFLAG_RD, 0, "Reservation Info");

/* add statistics which counts how many pages are zeroed in reservation for
 * early promotion at idle time
 */
static int async_prezero = 0, numofdeadbeef = 0, async_skipzero = 0;
SYSCTL_INT(_vm_reserv, OID_AUTO, async_prezero, CTLFLAG_RD,
    &async_prezero, 0, "Pages prezeroed for early promotion at idle time");
SYSCTL_INT(_vm_reserv, OID_AUTO, async_skipzero, CTLFLAG_RD,
    &async_skipzero, 0, "Pages skipped for zero in async promotion");
SYSCTL_INT(_vm_reserv, OID_AUTO, numofdeadbeef, CTLFLAG_RD,
    &numofdeadbeef, 0, "reservations need to migrate");

/* tunable parameters for asyncpromo */
static int enable_prezero = 0;
SYSCTL_INT(_vm_reserv, OID_AUTO, enable_prezero, CTLFLAG_RWTUN,
    &enable_prezero, 0, "enable prezeroing pages in reservation");

static int enable_compact = 0;
SYSCTL_INT(_vm_reserv, OID_AUTO, enable_compact, CTLFLAG_RWTUN,
    &enable_compact, 0, "enable evicting pages from inactive reservations");

static int enable_sleep = 1;
SYSCTL_INT(_vm_reserv, OID_AUTO, enable_sleep, CTLFLAG_RWTUN,
    &enable_sleep, 0, "enable asyncpromo daemon to sleep");

static int wakeup_frequency = 1;
SYSCTL_INT(_vm_reserv, OID_AUTO, wakeup_frequency, CTLFLAG_RWTUN,
    &wakeup_frequency, 0, "asyncpromo wakeup frequency");

static int wakeup_time = 1;
SYSCTL_INT(_vm_reserv, OID_AUTO, wakeup_time, CTLFLAG_RWTUN,
    &wakeup_time, 0, "asyncpromo wakeup time");

static int verbose = 0;
SYSCTL_INT(_vm_reserv, OID_AUTO, verbose, CTLFLAG_RWTUN,
    &verbose, 0, "asyncpromo verbose");

static int pop_budget = 2;
SYSCTL_INT(_vm_reserv, OID_AUTO, pop_budget, CTLFLAG_RWTUN,
    &pop_budget, 0, "asyncpromo pre-population budget");

/*
 * The actual threshold is 64,
 * because the 64th page fault will create a sp
 */
static int sync_popthreshold = 31;
SYSCTL_INT(_vm_reserv, OID_AUTO, sync_popthreshold, CTLFLAG_RWTUN,
    &sync_popthreshold, 0, "sync promotion pop threshold");

static int pop_threshold = 63;
SYSCTL_INT(_vm_reserv, OID_AUTO, pop_threshold, CTLFLAG_RWTUN,
    &pop_threshold, 0, "asyncpromo pop-x threshold");

static int zero_budget = 512;
SYSCTL_INT(_vm_reserv, OID_AUTO, zero_budget, CTLFLAG_RWTUN,
    &zero_budget, 0, "asyncpromo page zero budget");

static int pop_succ = 0, pop_fail = 0, pop_broken = 0;
SYSCTL_INT(_vm_reserv, OID_AUTO, pop_succ, CTLFLAG_RWTUN,
    &pop_succ, 0, "asyncpromo pop succ");
SYSCTL_INT(_vm_reserv, OID_AUTO, pop_fail, CTLFLAG_RWTUN,
    &pop_fail, 0, "asyncpromo pop fail");
SYSCTL_INT(_vm_reserv, OID_AUTO, pop_broken, CTLFLAG_RWTUN,
    &pop_broken, 0, "asyncpromo pop broken");;


static long vm_reserv_broken;
SYSCTL_LONG(_vm_reserv, OID_AUTO, broken, CTLFLAG_RD,
    &vm_reserv_broken, 0, "Cumulative number of broken reservations");

static long vm_reserv_freed;
SYSCTL_LONG(_vm_reserv, OID_AUTO, freed, CTLFLAG_RD,
    &vm_reserv_freed, 0, "Cumulative number of freed reservations");

static int sysctl_vm_reserv_freesp(SYSCTL_HANDLER_ARGS);

SYSCTL_PROC(_vm_reserv, OID_AUTO, freesp, CTLTYPE_INT | CTLFLAG_RD, NULL, 0,
    sysctl_vm_reserv_freesp, "I", "Current number of available free reservations");

static int sysctl_vm_reserv_fullpop(SYSCTL_HANDLER_ARGS);

SYSCTL_PROC(_vm_reserv, OID_AUTO, fullpop, CTLTYPE_INT | CTLFLAG_RD, NULL, 0,
    sysctl_vm_reserv_fullpop, "I", "Current number of full reservations");

static int sysctl_vm_reserv_partpopq(SYSCTL_HANDLER_ARGS);

SYSCTL_OID(_vm_reserv, OID_AUTO, partpopq, CTLTYPE_STRING | CTLFLAG_RD, NULL, 0,
    sysctl_vm_reserv_partpopq, "A", "Partially populated reservation queues");

static int sysctl_vm_reserv_popcdf(SYSCTL_HANDLER_ARGS);

SYSCTL_OID(_vm_reserv, OID_AUTO, popcdf, CTLTYPE_STRING | CTLFLAG_RD, NULL, 0,
    sysctl_vm_reserv_popcdf, "A", "Population CDF of existing reservations");

static int sysctl_vm_reserv_need_migrate(SYSCTL_HANDLER_ARGS);

SYSCTL_OID(_vm_reserv, OID_AUTO, need_migrate, CTLTYPE_STRING | CTLFLAG_RD, NULL, 0,
    sysctl_vm_reserv_need_migrate, "A", "Reservations who need page migration");

static long vm_reserv_reclaimed;
SYSCTL_LONG(_vm_reserv, OID_AUTO, reclaimed, CTLFLAG_RD,
    &vm_reserv_reclaimed, 0, "Cumulative number of reclaimed reservations");

static void		vm_reserv_break(vm_reserv_t rv);
static void		vm_reserv_depopulate(vm_reserv_t rv, int index);
static vm_reserv_t	vm_reserv_from_page(vm_page_t m);
static boolean_t	vm_reserv_has_pindex(vm_reserv_t rv,
			    vm_pindex_t pindex);
static void		vm_reserv_populate(vm_reserv_t rv, int index);
static void		vm_reserv_reclaim(vm_reserv_t rv);

/*
 * Returns the number of free physical superpages
 */
static int
sysctl_vm_reserv_freesp(SYSCTL_HANDLER_ARGS)
{
	int freesp = vm_phys_count_order_9();
	return (sysctl_handle_int(oidp, &freesp, 0, req));
}
/*
 * Returns the current number of full reservations.
 *
 * Since the number of full reservations is computed without acquiring the
 * free page queue lock, the returned value may be inexact.
 */
static int
sysctl_vm_reserv_fullpop(SYSCTL_HANDLER_ARGS)
{
	vm_paddr_t paddr;
	struct vm_phys_seg *seg;
	vm_reserv_t rv;
	int fullpop, segind;

	fullpop = 0;
	for (segind = 0; segind < vm_phys_nsegs; segind++) {
		seg = &vm_phys_segs[segind];
		paddr = roundup2(seg->start, VM_LEVEL_0_SIZE);
		while (paddr + VM_LEVEL_0_SIZE <= seg->end) {
			rv = &vm_reserv_array[paddr >> VM_LEVEL_0_SHIFT];
			fullpop += rv->popcnt == VM_LEVEL_0_NPAGES;
			paddr += VM_LEVEL_0_SIZE;
		}
	}
	return (sysctl_handle_int(oidp, &fullpop, 0, req));
}

/*
 * Describes the current state of the partially populated reservation queue.
 */
static int
sysctl_vm_reserv_partpopq(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sbuf;
	vm_reserv_t rv;
	int counter, error, level, unused_pages;

	error = sysctl_wire_old_buffer(req, 0);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sbuf, NULL, 128, req);
	sbuf_printf(&sbuf, "\nLEVEL     SIZE  NUMBER\n\n");
	for (level = -1; level <= VM_NRESERVLEVEL - 2; level++) {
		counter = 0;
		unused_pages = 0;
		mtx_lock(&vm_page_queue_free_mtx);
		TAILQ_FOREACH(rv, &vm_rvq_partpop/*[level]*/, partpopq) {
			/* clean side effect of using async_marker */
			if(rv->inpartpopq & RV_MARKER)
				continue;

			counter++;
			unused_pages += VM_LEVEL_0_NPAGES - rv->popcnt;
		}
		mtx_unlock(&vm_page_queue_free_mtx);
		sbuf_printf(&sbuf, "%5d: %6dK, %6d\n", level,
		    unused_pages * ((int)PAGE_SIZE / 1024), counter);
	}
	error = sbuf_finish(&sbuf);
	sbuf_delete(&sbuf);
	return (error);
}

/*
 * Describes the cdf of existing reservations
 */
static int
sysctl_vm_reserv_popcdf(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sbuf;
	vm_reserv_t rv;
	vm_paddr_t paddr;
	struct vm_phys_seg *seg;
	int fullpop, segind;
	int error, level, i;
	int cdf[513];

	error = sysctl_wire_old_buffer(req, 0);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sbuf, NULL, 4096, req);
	sbuf_printf(&sbuf, "\n");

	for(i = 0; i < 512; i ++)
		cdf[i] = 0;
	for (level = -1; level <= VM_NRESERVLEVEL - 2; level++) {
		mtx_lock(&vm_page_queue_free_mtx);
		TAILQ_FOREACH(rv, &vm_rvq_partpop/*[level]*/, partpopq) {
			/* clean side effect of using rv_marker */
			if(rv->inpartpopq & RV_MARKER)
				continue;
			cdf[rv->popcnt] ++;
		}
		mtx_unlock(&vm_page_queue_free_mtx);
	}


	fullpop = 0;
	for (segind = 0; segind < vm_phys_nsegs; segind++) {
		seg = &vm_phys_segs[segind];
		paddr = roundup2(seg->start, VM_LEVEL_0_SIZE);
		while (paddr + VM_LEVEL_0_SIZE <= seg->end) {
			rv = &vm_reserv_array[paddr >> VM_LEVEL_0_SHIFT];
			fullpop += rv->popcnt == VM_LEVEL_0_NPAGES;
			paddr += VM_LEVEL_0_SIZE;
		}
	}
	cdf[512] = fullpop;
	for(i = 1; i < 513; i ++)
		sbuf_printf(&sbuf, "%d ", cdf[i]);
	sbuf_printf(&sbuf, "\n");

	error = sbuf_finish(&sbuf);
	sbuf_delete(&sbuf);
	return (error);
}

/*
 * Describes the current state of the partially populated reservation queue.
 */
static int
sysctl_vm_reserv_need_migrate(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sbuf;
	vm_reserv_t rv;
	int counter, error, level;

	error = sysctl_wire_old_buffer(req, 0);
	if (error != 0)
		return (error);
	sbuf_new_for_sysctl(&sbuf, NULL, 128, req);
	sbuf_printf(&sbuf, "\nLEVEL     NUMBER\n\n");
	for (level = -1; level <= VM_NRESERVLEVEL - 2; level++) {
		counter = 0;
		mtx_lock(&vm_page_queue_free_mtx);
		TAILQ_FOREACH(rv, &vm_rvq_partpop/*[level]*/, partpopq) {
			/* clean side effect of using rv_marker */
			if(rv->inpartpopq & RV_MARKER)
				continue;
			if(rv->inpartpopq & RV_NEEDMIGRATE)
				counter++;
		}
		mtx_unlock(&vm_page_queue_free_mtx);
		sbuf_printf(&sbuf, "%5d: %6d\n", level, counter);
	}
	error = sbuf_finish(&sbuf);
	sbuf_delete(&sbuf);
	return (error);
}

/*
 * Reduces the given reservation's population count.  If the population count
 * becomes zero, the reservation is destroyed.  Additionally, moves the
 * reservation to the tail of the partially populated reservation queue if the
 * population count is non-zero.
 *
 * The free page queue lock must be held.
 */
static void
vm_reserv_depopulate(vm_reserv_t rv, int index)
{

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	KASSERT(rv->object != NULL,
	    ("vm_reserv_depopulate: reserv %p is free", rv));
	KASSERT(popmap_is_set(rv->popmap, index),
	    ("vm_reserv_depopulate: reserv %p's popmap[%d] is clear", rv,
	    index));
	KASSERT(rv->popcnt > 0,
	    ("vm_reserv_depopulate: reserv %p's popcnt is corrupted", rv));
	if (is_inpartpopq(rv->inpartpopq)) {
		TAILQ_REMOVE(&vm_rvq_partpop, rv, partpopq);
		clear_inpartpopq(rv->inpartpopq);
	} else {
		KASSERT(rv->pages->psind == 1,
		    ("vm_reserv_depopulate: reserv %p is already demoted",
		    rv));
		rv->pages->psind = 0;
	}
	popmap_clear(rv->popmap, index);
	rv->popcnt--;
	if (rv->popcnt == 0) {
		LIST_REMOVE(rv, objq);
		clear_all(rv->inpartpopq);
		rv->object = NULL;
		vm_phys_free_pages(rv->pages, VM_LEVEL_0_ORDER);
		vm_reserv_freed++;
	} else {
		/* maybe prepopulation should skip badboy */
		set_inpartpopq(rv->inpartpopq);
		rv->timestamp = ticks;
		TAILQ_INSERT_TAIL(&vm_rvq_partpop, rv, partpopq);
	}
}

/*
 * Returns the reservation to which the given page might belong.
 */
static __inline vm_reserv_t
vm_reserv_from_page(vm_page_t m)
{

	return (&vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT]);
}

/*
 * Returns TRUE if the given reservation contains the given page index and
 * FALSE otherwise.
 */
static __inline boolean_t
vm_reserv_has_pindex(vm_reserv_t rv, vm_pindex_t pindex)
{

	return (((pindex - rv->pindex) & ~(VM_LEVEL_0_NPAGES - 1)) == 0);
}

/* [syncpromo]
	functions to help vm_fault syncronously promote superpages
 */
bool
vm_reserv_satisfy_sync_promotion(vm_page_t m)
{
	vm_reserv_t rv = &vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT];
	/* expect m to be installed in a reservation */
	if(rv->object != NULL && rv->object == m->object
		&& m->pindex >= rv->pindex
		&& m->pindex < rv->pindex + VM_LEVEL_0_NPAGES
		&& popmap_is_set(rv->popmap, m->pindex - rv->pindex)
		&& rv->inpartpopq == RV_INPARTPOPQ
		&& rv->popcnt >= sync_popthreshold)
		return TRUE;
	else
		return FALSE;
}

bool
vm_reserv_satisfy_adj_promotion(vm_page_t m)
{
	vm_reserv_t rv = &vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT];
	/* expect m to be installed in a reservation */
	if(rv->object != NULL && rv->object == m->object
		&& m->pindex >= rv->pindex
		&& m->pindex < rv->pindex + VM_LEVEL_0_NPAGES
		&& popmap_is_set(rv->popmap, m->pindex - rv->pindex)
		&& rv->inpartpopq == RV_INPARTPOPQ)
		return TRUE;
	else
		return FALSE;
}

vm_pindex_t
vm_reserv_pindex_from_page(vm_page_t m)
{
	return vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT].pindex;
}

void
vm_reserv_copy_popmap_from_page(vm_page_t m, popmap_t *popmap)
{
	int i;
	for(i = 0; i < NPOPMAP; i ++)
		popmap[i] = vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT].popmap[i];
}

int
vm_reserv_popmap_is_clear(vm_page_t m, int i)
{
	return popmap_is_clear(vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT].popmap, i);
}

int
vm_reserv_get_next_set_index(vm_page_t m, int i)
{
	vm_reserv_t rv = &vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT];
	while(popmap_is_clear(rv->popmap, i) && i < 512)
		i ++;
	return i;
}

int
vm_reserv_get_next_clear_index(vm_page_t m, int i)
{
	vm_reserv_t rv = &vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT];
	while(popmap_is_set(rv->popmap, i) && i < 512)
		i ++;
	return i;
}

void
vm_reserv_mark_bad(vm_page_t m)
{
	vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT].inpartpopq |= RV_BADBOY;
	return ;
}

bool
vm_reserv_is_full(vm_page_t m)
{
	return (vm_reserv_array[VM_PAGE_TO_PHYS(m) >> VM_LEVEL_0_SHIFT].popcnt == VM_LEVEL_0_NPAGES);
}

/*
 * Increases the given reservation's population count.  Moves the reservation
 * to the tail of the partially populated reservation queue.
 *
 * The free page queue must be locked.
 */
static void
vm_reserv_populate(vm_reserv_t rv, int index)
{

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	KASSERT(rv->object != NULL,
	    ("vm_reserv_populate: reserv %p is free", rv));
	KASSERT(popmap_is_clear(rv->popmap, index),
	    ("vm_reserv_populate: reserv %p's popmap[%d] is set", rv,
	    index));
	KASSERT(rv->popcnt < VM_LEVEL_0_NPAGES,
	    ("vm_reserv_populate: reserv %p is already full", rv));
	KASSERT(rv->pages->psind == 0,
	    ("vm_reserv_populate: reserv %p is already promoted", rv));
	if (is_inpartpopq(rv->inpartpopq)) {
		TAILQ_REMOVE(&vm_rvq_partpop, rv, partpopq);
		clear_inpartpopq(rv->inpartpopq);
	}
	popmap_set(rv->popmap, index);
	rv->popcnt++;
	if (rv->popcnt < VM_LEVEL_0_NPAGES) {
		set_inpartpopq(rv->inpartpopq);
		rv->timestamp = ticks;
		TAILQ_INSERT_TAIL(&vm_rvq_partpop, rv, partpopq);
	} else
		rv->pages->psind = 1;
}

/*
 * Allocates a contiguous set of physical pages of the given size "npages"
 * from existing or newly created reservations.  All of the physical pages
 * must be at or above the given physical address "low" and below the given
 * physical address "high".  The given value "alignment" determines the
 * alignment of the first physical page in the set.  If the given value
 * "boundary" is non-zero, then the set of physical pages cannot cross any
 * physical address boundary that is a multiple of that value.  Both
 * "alignment" and "boundary" must be a power of two.
 *
 * The page "mpred" must immediately precede the offset "pindex" within the
 * specified object.
 *
 * The object and free page queue must be locked.
 */
vm_page_t
vm_reserv_alloc_contig(vm_object_t object, vm_pindex_t pindex, u_long npages,
    vm_paddr_t low, vm_paddr_t high, u_long alignment, vm_paddr_t boundary,
    vm_page_t mpred)
{
	vm_paddr_t pa, size;
	vm_page_t m, m_ret, msucc;
	vm_pindex_t first, leftcap, rightcap;
	vm_reserv_t rv;
	u_long allocpages, maxpages, minpages;
	int i, index, n;

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	VM_OBJECT_ASSERT_WLOCKED(object);
	KASSERT(npages != 0, ("vm_reserv_alloc_contig: npages is 0"));

	/*
	 * Is a reservation fundamentally impossible?
	 */
	if (pindex < VM_RESERV_INDEX(object, pindex) ||
	    pindex + npages > object->size)
		return (NULL);

	/*
	 * All reservations of a particular size have the same alignment.
	 * Assuming that the first page is allocated from a reservation, the
	 * least significant bits of its physical address can be determined
	 * from its offset from the beginning of the reservation and the size
	 * of the reservation.
	 *
	 * Could the specified index within a reservation of the smallest
	 * possible size satisfy the alignment and boundary requirements?
	 */
	pa = VM_RESERV_INDEX(object, pindex) << PAGE_SHIFT;
	if ((pa & (alignment - 1)) != 0)
		return (NULL);
	size = npages << PAGE_SHIFT;
	if (((pa ^ (pa + size - 1)) & ~(boundary - 1)) != 0)
		return (NULL);

	/*
	 * Look for an existing reservation.
	 */
	if (mpred != NULL) {
		KASSERT(mpred->object == object,
		    ("vm_reserv_alloc_contig: object doesn't contain mpred"));
		KASSERT(mpred->pindex < pindex,
		    ("vm_reserv_alloc_contig: mpred doesn't precede pindex"));
		rv = vm_reserv_from_page(mpred);
		if (rv->object == object && vm_reserv_has_pindex(rv, pindex))
			goto found;
		msucc = TAILQ_NEXT(mpred, listq);
	} else
		msucc = TAILQ_FIRST(&object->memq);
	if (msucc != NULL) {
		KASSERT(msucc->pindex > pindex,
		    ("vm_reserv_alloc_contig: msucc doesn't succeed pindex"));
		rv = vm_reserv_from_page(msucc);
		if (rv->object == object && vm_reserv_has_pindex(rv, pindex))
			goto found;
	}

	/*
	 * Could at least one reservation fit between the first index to the
	 * left that can be used ("leftcap") and the first index to the right
	 * that cannot be used ("rightcap")?
	 */
	first = pindex - VM_RESERV_INDEX(object, pindex);
	if (mpred != NULL) {
		if ((rv = vm_reserv_from_page(mpred))->object != object)
			leftcap = mpred->pindex + 1;
		else
			leftcap = rv->pindex + VM_LEVEL_0_NPAGES;
		if (leftcap > first)
			return (NULL);
	}
	minpages = VM_RESERV_INDEX(object, pindex) + npages;
	maxpages = roundup2(minpages, VM_LEVEL_0_NPAGES);
	allocpages = maxpages;
	if (msucc != NULL) {
		if ((rv = vm_reserv_from_page(msucc))->object != object)
			rightcap = msucc->pindex;
		else
			rightcap = rv->pindex;
		if (first + maxpages > rightcap) {
			if (maxpages == VM_LEVEL_0_NPAGES)
				return (NULL);

			/*
			 * At least one reservation will fit between "leftcap"
			 * and "rightcap".  However, a reservation for the
			 * last of the requested pages will not fit.  Reduce
			 * the size of the upcoming allocation accordingly.
			 */
			allocpages = minpages;
		}
	}

	/*
	 * Would the last new reservation extend past the end of the object?
	 */
	if (first + maxpages > object->size) {
		/*
		 * Don't allocate the last new reservation if the object is a
		 * vnode or backed by another object that is a vnode. 
		 */
		if (object->type == OBJT_VNODE ||
		    (object->backing_object != NULL &&
		    object->backing_object->type == OBJT_VNODE)) {
			if (maxpages == VM_LEVEL_0_NPAGES)
				return (NULL);
			allocpages = minpages;
		}
		/* Speculate that the object may grow. */
	}

	/*
	 * Allocate the physical pages.  The alignment and boundary specified
	 * for this allocation may be different from the alignment and
	 * boundary specified for the requested pages.  For instance, the
	 * specified index may not be the first page within the first new
	 * reservation.
	 */
	m = vm_phys_alloc_contig(allocpages, low, high, ulmax(alignment,
	    VM_LEVEL_0_SIZE), boundary > VM_LEVEL_0_SIZE ? boundary : 0);
	if (m == NULL)
		return (NULL);

	/*
	 * The allocated physical pages always begin at a reservation
	 * boundary, but they do not always end at a reservation boundary.
	 * Initialize every reservation that is completely covered by the
	 * allocated physical pages.
	 */
	m_ret = NULL;
	index = VM_RESERV_INDEX(object, pindex);
	do {
		rv = vm_reserv_from_page(m);
		KASSERT(rv->pages == m,
		    ("vm_reserv_alloc_contig: reserv %p's pages is corrupted",
		    rv));
		KASSERT(rv->object == NULL,
		    ("vm_reserv_alloc_contig: reserv %p isn't free", rv));
		LIST_INSERT_HEAD(&object->rvq, rv, objq);
		rv->object = object;
		rv->pindex = first;
		KASSERT(rv->popcnt == 0,
		    ("vm_reserv_alloc_contig: reserv %p's popcnt is corrupted",
		    rv));
		KASSERT(!is_inpartpopq(rv->inpartpopq),
		    ("vm_reserv_alloc_contig: reserv %p's inpartpopq is TRUE",
		    rv));
		for (i = 0; i < NPOPMAP; i++)
			KASSERT(rv->popmap[i] == 0,
		    ("vm_reserv_alloc_contig: reserv %p's popmap is corrupted",
			    rv));
		n = ulmin(VM_LEVEL_0_NPAGES - index, npages);
		for (i = 0; i < n; i++)
			vm_reserv_populate(rv, index + i);
		npages -= n;
		if (m_ret == NULL) {
			m_ret = &rv->pages[index];
			index = 0;
		}
		m += VM_LEVEL_0_NPAGES;
		first += VM_LEVEL_0_NPAGES;
		allocpages -= VM_LEVEL_0_NPAGES;
	} while (allocpages >= VM_LEVEL_0_NPAGES);
	return (m_ret);

	/*
	 * Found a matching reservation.
	 */
found:
	index = VM_RESERV_INDEX(object, pindex);
	/* Does the allocation fit within the reservation? */
	if (index + npages > VM_LEVEL_0_NPAGES)
		return (NULL);
	m = &rv->pages[index];
	pa = VM_PAGE_TO_PHYS(m);
	if (pa < low || pa + size > high || (pa & (alignment - 1)) != 0 ||
	    ((pa ^ (pa + size - 1)) & ~(boundary - 1)) != 0)
		return (NULL);
	/* Handle vm_page_rename(m, new_object, ...). */
	for (i = 0; i < npages; i++)
		if (popmap_is_set(rv->popmap, index + i))
			return (NULL);
	for (i = 0; i < npages; i++)
		vm_reserv_populate(rv, index + i);
	return (m);
}

/*
 * Allocates a page from an existing or newly created reservation.
 *
 * The page "mpred" must immediately precede the offset "pindex" within the
 * specified object.
 *
 * The object and free page queue must be locked.
 */
vm_page_t
vm_reserv_alloc_page(vm_object_t object, vm_pindex_t pindex, vm_page_t mpred)
{
	vm_page_t m, msucc;
	vm_pindex_t first, leftcap, rightcap;
	vm_reserv_t rv;
	int i, index;

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	VM_OBJECT_ASSERT_WLOCKED(object);

	/*
	 * Is a reservation fundamentally impossible?
	 */
	if (pindex < VM_RESERV_INDEX(object, pindex) ||
	    pindex >= object->size)
		return (NULL);

	/*
	 * Look for an existing reservation.
	 */
	if (mpred != NULL) {
		KASSERT(mpred->object == object,
		    ("vm_reserv_alloc_page: object doesn't contain mpred"));
		KASSERT(mpred->pindex < pindex,
		    ("vm_reserv_alloc_page: mpred doesn't precede pindex"));
		rv = vm_reserv_from_page(mpred);
		if (rv->object == object && vm_reserv_has_pindex(rv, pindex))
			goto found;
		msucc = TAILQ_NEXT(mpred, listq);
	} else
		msucc = TAILQ_FIRST(&object->memq);
	if (msucc != NULL) {
		KASSERT(msucc->pindex > pindex,
		    ("vm_reserv_alloc_page: msucc doesn't succeed pindex"));
		rv = vm_reserv_from_page(msucc);
		if (rv->object == object && vm_reserv_has_pindex(rv, pindex))
			goto found;
	}

	/*
	 * Could a reservation fit between the first index to the left that
	 * can be used and the first index to the right that cannot be used?
	 */
	first = pindex - VM_RESERV_INDEX(object, pindex);
	if (mpred != NULL) {
		if ((rv = vm_reserv_from_page(mpred))->object != object)
			leftcap = mpred->pindex + 1;
		else
			leftcap = rv->pindex + VM_LEVEL_0_NPAGES;
		if (leftcap > first)
			return (NULL);
	}
	if (msucc != NULL) {
		if ((rv = vm_reserv_from_page(msucc))->object != object)
			rightcap = msucc->pindex;
		else
			rightcap = rv->pindex;
		if (first + VM_LEVEL_0_NPAGES > rightcap)
			return (NULL);
	}

	/*
	 * Would a new reservation extend past the end of the object? 
	 */
	if (first + VM_LEVEL_0_NPAGES > object->size) {
		/*
		 * Don't allocate a new reservation if the object is a vnode or
		 * backed by another object that is a vnode. 
		 */
		if (object->type == OBJT_VNODE ||
		    (object->backing_object != NULL &&
		    object->backing_object->type == OBJT_VNODE))
			return (NULL);
		/* Speculate that the object may grow. */
	}

	/*
	 * Allocate and populate the new reservation.
	 */
	m = vm_phys_alloc_pages(VM_FREEPOOL_DEFAULT, VM_LEVEL_0_ORDER);
	if (m == NULL)
		return (NULL);
	rv = vm_reserv_from_page(m);
	KASSERT(rv->pages == m,
	    ("vm_reserv_alloc_page: reserv %p's pages is corrupted", rv));
	KASSERT(rv->object == NULL,
	    ("vm_reserv_alloc_page: reserv %p isn't free", rv));
	LIST_INSERT_HEAD(&object->rvq, rv, objq);
	rv->object = object;
	rv->pindex = first;
	KASSERT(rv->popcnt == 0,
	    ("vm_reserv_alloc_page: reserv %p's popcnt is corrupted", rv));
	KASSERT(!is_inpartpopq(rv->inpartpopq),
	    ("vm_reserv_alloc_page: reserv %p's inpartpopq is TRUE", rv));
	for (i = 0; i < NPOPMAP; i++)
		KASSERT(rv->popmap[i] == 0,
		    ("vm_reserv_alloc_page: reserv %p's popmap is corrupted",
		    rv));
	index = VM_RESERV_INDEX(object, pindex);
	vm_reserv_populate(rv, index);
	return (&rv->pages[index]);

	/*
	 * Found a matching reservation.
	 */
found:
	index = VM_RESERV_INDEX(object, pindex);
	m = &rv->pages[index];
	/* Handle vm_page_rename(m, new_object, ...). */
	if (popmap_is_set(rv->popmap, index))
		return (NULL);
	vm_reserv_populate(rv, index);
	return (m);
}

/*
 * Breaks the given reservation.  All free pages in the reservation
 * are returned to the physical memory allocator.  The reservation's
 * population count and map are reset to their initial state.
 *
 * The given reservation must not be in the partially populated reservation
 * queue.  The free page queue lock must be held.
 */
static void
vm_reserv_break(vm_reserv_t rv)
{
	int begin_zeroes, hi, i, lo;

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	KASSERT(rv->object != NULL,
	    ("vm_reserv_break: reserv %p is free", rv));
	KASSERT(!is_inpartpopq(rv->inpartpopq),
	    ("vm_reserv_break: reserv %p's inpartpopq is TRUE", rv));
	LIST_REMOVE(rv, objq);
	rv->object = NULL;
	rv->pages->psind = 0;
	i = hi = 0;
	do {
		/* Find the next 0 bit.  Any previous 0 bits are < "hi". */
		lo = ffsl(~(((1UL << hi) - 1) | rv->popmap[i]));
		if (lo == 0) {
			/* Redundantly clears bits < "hi". */
			rv->popmap[i] = 0;
			rv->popcnt -= NBPOPMAP - hi;
			while (++i < NPOPMAP) {
				lo = ffsl(~rv->popmap[i]);
				if (lo == 0) {
					rv->popmap[i] = 0;
					rv->popcnt -= NBPOPMAP;
				} else
					break;
			}
			if (i == NPOPMAP)
				break;
			hi = 0;
		}
		KASSERT(lo > 0, ("vm_reserv_break: lo is %d", lo));
		/* Convert from ffsl() to ordinary bit numbering. */
		lo--;
		if (lo > 0) {
			/* Redundantly clears bits < "hi". */
			rv->popmap[i] &= ~((1UL << lo) - 1);
			rv->popcnt -= lo - hi;
		}
		begin_zeroes = NBPOPMAP * i + lo;
		/* Find the next 1 bit. */
		do
			hi = ffsl(rv->popmap[i]);
		while (hi == 0 && ++i < NPOPMAP);
		if (i != NPOPMAP)
			/* Convert from ffsl() to ordinary bit numbering. */
			hi--;
		vm_phys_free_contig(&rv->pages[begin_zeroes], NBPOPMAP * i +
		    hi - begin_zeroes);
	} while (i < NPOPMAP);
	KASSERT(rv->popcnt == 0,
	    ("vm_reserv_break: reserv %p's popcnt is corrupted", rv));
	vm_reserv_broken++;
}

/*
 * Breaks all reservations belonging to the given object.
 */
void
vm_reserv_break_all(vm_object_t object)
{
	vm_reserv_t rv;

	mtx_lock(&vm_page_queue_free_mtx);
	while ((rv = LIST_FIRST(&object->rvq)) != NULL) {
		KASSERT(rv->object == object,
		    ("vm_reserv_break_all: reserv %p is corrupted", rv));
		if (is_inpartpopq(rv->inpartpopq)) {
			TAILQ_REMOVE(&vm_rvq_partpop, rv, partpopq);
		}
		clear_all(rv->inpartpopq);
		vm_reserv_break(rv);
	}
	mtx_unlock(&vm_page_queue_free_mtx);
}

/*
 * Frees the given page if it belongs to a reservation.  Returns TRUE if the
 * page is freed and FALSE otherwise.
 *
 * The free page queue lock must be held.
 */
boolean_t
vm_reserv_free_page(vm_page_t m)
{
	vm_reserv_t rv;

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	rv = vm_reserv_from_page(m);
	if (rv->object == NULL)
		return (FALSE);
	vm_reserv_depopulate(rv, m - rv->pages);
	return (TRUE);
}

/*
 * Initializes the reservation management system.  Specifically, initializes
 * the reservation array.
 *
 * Requires that vm_page_array and first_page are initialized!
 */
void
vm_reserv_init(void)
{
	vm_paddr_t paddr;
	struct vm_phys_seg *seg;
	int segind;

	/*
	 * Initialize the reservation array.  Specifically, initialize the
	 * "pages" field for every element that has an underlying superpage.
	 */
	for (segind = 0; segind < vm_phys_nsegs; segind++) {
		seg = &vm_phys_segs[segind];
		paddr = roundup2(seg->start, VM_LEVEL_0_SIZE);
		while (paddr + VM_LEVEL_0_SIZE <= seg->end) {
			vm_reserv_array[paddr >> VM_LEVEL_0_SHIFT].pages =
			    PHYS_TO_VM_PAGE(paddr);
			paddr += VM_LEVEL_0_SIZE;
		}
	}
}

/*
 * Returns true if the given page belongs to a reservation and that page is
 * free.  Otherwise, returns false.
 */
bool
vm_reserv_is_page_free(vm_page_t m)
{
	vm_reserv_t rv;

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	rv = vm_reserv_from_page(m);
	if (rv->object == NULL)
		return (false);
	return (popmap_is_clear(rv->popmap, m - rv->pages));
}

/*
 * If the given page belongs to a reservation, returns the level of that
 * reservation.  Otherwise, returns -1.
 */
int
vm_reserv_level(vm_page_t m)
{
	vm_reserv_t rv;

	rv = vm_reserv_from_page(m);
	return (rv->object != NULL ? 0 : -1);
}

/*
 * Returns a reservation level if the given page belongs to a fully populated
 * reservation and -1 otherwise.
 */
int
vm_reserv_level_iffullpop(vm_page_t m)
{
	vm_reserv_t rv;

	rv = vm_reserv_from_page(m);
	return (rv->popcnt == VM_LEVEL_0_NPAGES ? 0 : -1);
}

/*
 * Breaks the given partially populated reservation, releasing its free pages
 * to the physical memory allocator.
 *
 * The free page queue lock must be held.
 */
static void
vm_reserv_reclaim(vm_reserv_t rv)
{

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	KASSERT(is_inpartpopq(rv->inpartpopq),
	    ("vm_reserv_reclaim: reserv %p's inpartpopq is FALSE", rv));
	TAILQ_REMOVE(&vm_rvq_partpop, rv, partpopq);
	clear_all(rv->inpartpopq);
	vm_reserv_break(rv);
	vm_reserv_reclaimed++;
}

/*
 * Breaks the reservation at the head of the partially populated reservation
 * queue, releasing its free pages to the physical memory allocator.  Returns
 * TRUE if a reservation is broken and FALSE otherwise.
 *
 * The free page queue lock must be held.
 */
boolean_t
vm_reserv_reclaim_inactive(void)
{
	vm_reserv_t rv;

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	rv = TAILQ_FIRST(&vm_rvq_partpop);
	if(rv->inpartpopq & RV_MARKER)
		rv = TAILQ_NEXT(rv, partpopq);
	if (rv != NULL) {
		vm_reserv_reclaim(rv);
		return (TRUE);
	}
	return (FALSE);
}

/*
 * Searches the partially populated reservation queue for the least recently
 * changed reservation with free pages that satisfy the given request for
 * contiguous physical memory.  If a satisfactory reservation is found, it is
 * broken.  Returns TRUE if a reservation is broken and FALSE otherwise.
 *
 * The free page queue lock must be held.
 */
boolean_t
vm_reserv_reclaim_contig(u_long npages, vm_paddr_t low, vm_paddr_t high,
    u_long alignment, vm_paddr_t boundary)
{
	vm_paddr_t pa, size;
	vm_reserv_t rv;
	int hi, i, lo, low_index, next_free;

	mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
	if (npages > VM_LEVEL_0_NPAGES - 1)
		return (FALSE);
	size = npages << PAGE_SHIFT;
	TAILQ_FOREACH(rv, &vm_rvq_partpop, partpopq) {
		/* clean side effect of using aync_marker */
		if(rv->inpartpopq & RV_MARKER)
			continue;

		pa = VM_PAGE_TO_PHYS(&rv->pages[VM_LEVEL_0_NPAGES - 1]);
		if (pa + PAGE_SIZE - size < low) {
			/* This entire reservation is too low; go to next. */
			continue;
		}
		pa = VM_PAGE_TO_PHYS(&rv->pages[0]);
		if (pa + size > high) {
			/* This entire reservation is too high; go to next. */
			continue;
		}
		if (pa < low) {
			/* Start the search for free pages at "low". */
			low_index = (low + PAGE_MASK - pa) >> PAGE_SHIFT;
			i = low_index / NBPOPMAP;
			hi = low_index % NBPOPMAP;
		} else
			i = hi = 0;
		do {
			/* Find the next free page. */
			lo = ffsl(~(((1UL << hi) - 1) | rv->popmap[i]));
			while (lo == 0 && ++i < NPOPMAP)
				lo = ffsl(~rv->popmap[i]);
			if (i == NPOPMAP)
				break;
			/* Convert from ffsl() to ordinary bit numbering. */
			lo--;
			next_free = NBPOPMAP * i + lo;
			pa = VM_PAGE_TO_PHYS(&rv->pages[next_free]);
			KASSERT(pa >= low,
			    ("vm_reserv_reclaim_contig: pa is too low"));
			if (pa + size > high) {
				/* The rest of this reservation is too high. */
				break;
			} else if ((pa & (alignment - 1)) != 0 ||
			    ((pa ^ (pa + size - 1)) & ~(boundary - 1)) != 0) {
				/*
				 * The current page doesn't meet the alignment
				 * and/or boundary requirements.  Continue
				 * searching this reservation until the rest
				 * of its free pages are either excluded or
				 * exhausted.
				 */
				hi = lo + 1;
				if (hi >= NBPOPMAP) {
					hi = 0;
					i++;
				}
				continue;
			}
			/* Find the next used page. */
			hi = ffsl(rv->popmap[i] & ~((1UL << lo) - 1));
			while (hi == 0 && ++i < NPOPMAP) {
				if ((NBPOPMAP * i - next_free) * PAGE_SIZE >=
				    size) {
					vm_reserv_reclaim(rv);
					return (TRUE);
				}
				hi = ffsl(rv->popmap[i]);
			}
			/* Convert from ffsl() to ordinary bit numbering. */
			if (i != NPOPMAP)
				hi--;
			if ((NBPOPMAP * i + hi - next_free) * PAGE_SIZE >=
			    size) {
				vm_reserv_reclaim(rv);
				return (TRUE);
			}
		} while (i < NPOPMAP);
	}
	return (FALSE);
}

/*
 * Transfers the reservation underlying the given page to a new object.
 *
 * The object must be locked.
 */
void
vm_reserv_rename(vm_page_t m, vm_object_t new_object, vm_object_t old_object,
    vm_pindex_t old_object_offset)
{
	vm_reserv_t rv, rv_tmp;
	boolean_t merge;

	VM_OBJECT_ASSERT_WLOCKED(new_object);
	rv = vm_reserv_from_page(m);
	if (rv->object == old_object) {
		mtx_lock(&vm_page_queue_free_mtx);
		if (rv->object == old_object) {
			LIST_REMOVE(rv, objq);
			LIST_INSERT_HEAD(&new_object->rvq, rv, objq);
			rv->object = new_object;
			rv->pindex -= old_object_offset;

			/*
			 * [asyncpromo] Check pindex collision in &new_object->rvq
			 * Not sure if it is worth, as vm_page_queue_free_mtx is locked
			 */
			merge = FALSE;
			rv_tmp = LIST_NEXT(rv, objq);
			if(rv_tmp != NULL)
				/* scan the rest of objq from 2nd element, if size >= 2 */
				LIST_FOREACH_FROM(rv_tmp, &new_object->rvq, objq)
					if(rv_tmp->pindex == rv->pindex)
					{
						set_migrate(rv_tmp->inpartpopq);
						merge = TRUE;
					}
			if(merge)
				set_migrate(rv->inpartpopq);
		}
		mtx_unlock(&vm_page_queue_free_mtx);
	}
}

/*
 * Returns the size (in bytes) of a reservation of the specified level.
 */
int
vm_reserv_size(int level)
{

	switch (level) {
	case 0:
		return (VM_LEVEL_0_SIZE);
	case -1:
		return (PAGE_SIZE);
	default:
		return (0);
	}
}

/*
 * Allocates the virtual and physical memory required by the reservation
 * management system's data structures, in particular, the reservation array.
 */
vm_paddr_t
vm_reserv_startup(vm_offset_t *vaddr, vm_paddr_t end, vm_paddr_t high_water)
{
	vm_paddr_t new_end;
	size_t size;

	/*
	 * Calculate the size (in bytes) of the reservation array.  Round up
	 * from "high_water" because every small page is mapped to an element
	 * in the reservation array based on its physical address.  Thus, the
	 * number of elements in the reservation array can be greater than the
	 * number of superpages. 
	 */
	size = howmany(high_water, VM_LEVEL_0_SIZE) * sizeof(struct vm_reserv);

	/*
	 * Allocate and map the physical memory for the reservation array.  The
	 * next available virtual address is returned by reference.
	 */
	new_end = end - round_page(size);
	vm_reserv_array = (void *)(uintptr_t)pmap_map(vaddr, new_end, end,
	    VM_PROT_READ | VM_PROT_WRITE);
	bzero(vm_reserv_array, size);

	/*
	 * Return the next available physical address.
	 */
	return (new_end);
}

/*
 * Returns the superpage containing the given page.
 */
vm_page_t
vm_reserv_to_superpage(vm_page_t m)
{
	vm_reserv_t rv;

	VM_OBJECT_ASSERT_LOCKED(m->object);
	rv = vm_reserv_from_page(m);
	return (rv->object == m->object && rv->popcnt == VM_LEVEL_0_NPAGES ?
	    rv->pages : NULL);
}

/*
 * Below is the code for async_promote daemon
 * The daemon should periodically scan partpopq to determine:
 */

/*
 * Implement the asynchronous early-promotion mechanism.
 */
static int async_promote;

/*
 * Check if rv can never get fully populated.
 * If a shadow object collapse, reservation could be renamed and merged into the private object,
 * where there could be existing reservation sharing the same pindex
 * The free page queue lock must be acquired, so do not use this function now
 */
static boolean_t
vm_reserv_is_deadbeef(vm_reserv_t rv)
{
	vm_object_t obj;
	vm_pindex_t pindex;
	vm_reserv_t rvi;

	obj = rv->object;
	pindex = rv->pindex;
	LIST_FOREACH(rvi, &obj->rvq, objq)
	{
		if(rvi != rv && rvi->pindex == pindex)
			return TRUE;
	}
	return FALSE;
}

/*
 * This function pre-populate and zero all remaining free pages in a reservation
 * Subsequent page fault will use pmap_enter to create a superpage directly
 * This function does not necesssarily succeed. The free page queue lock is
 * held only inside the vm_page_alloc when allocating a page from the reservation
 * For performance, it is much better not to hold the free page queue lock,
 * so, the vm_page_alloc may need to fail and detect if the reservation is broken,
 * and we cannot assume the reservation's object is going to be valid all the time.
 * The function would return whether prepopulate succeeds
 * Caution: rv can be broken at any time without holding free page queue lock
 * Return indicates whether the pre-population succeeds or not

 * If rv exists then populate and zero all pages in the reservation
 * This solution uses vm_page_alloc() to directly install all free pages
 * and then zero all these busy pages and release their busy lock.
 * After that, a cheap page fault on any page would promote this reservation as
 * a superpage.
 *
 * Possible Optimization:
 * Delete one page mapping to enforce a soft page fault to install a superpage
 * mapping.
 *
 */
static boolean_t
vm_reserv_prepopulate(vm_reserv_t rv)
{
	int i;
	vm_page_t m, mpred;
	vm_object_t object;

	mtx_assert(&vm_page_queue_free_mtx, MA_NOTOWNED);
	/* [race] rv may be broken here */
	if((object = rv->object) != NULL)
		VM_OBJECT_WLOCK(object);
	else
	{
		pop_broken ++;
		return FALSE;
	}

	mpred = vm_page_find_most(object, rv->pindex + VM_LEVEL_0_NPAGES - 1);
	/* 
	 * scan backwards
	 * maintain a vm_page_t cursor which is the last page I added
	 * with TAILQ_NEXT at the page to look at the pindex of the next page
	 * if it is equal to the index I am trying to prepopulate
	 */
	for(i = VM_LEVEL_0_NPAGES - 1; i >= 0; i --)
	{
		/* rv can be broken here, because free page queue is not locked */
		if(rv->object == NULL)
			goto fail;

		if(mpred != NULL && mpred->pindex == rv->pindex + i)
		{
			/* only examine consistency when mpred is reached, otherwise it is safe to prepopulate */
			if(popmap_is_set(rv->popmap, i))
			{
				if(mpred == &rv->pages[i])
					mpred = TAILQ_PREV(mpred, pglist, listq);
				else
					panic("%s: inconsistency found at pindex %lu: object holds 2 pages for the same pindex",
					    __func__, rv->pindex + i);
			}
			else
				/* mpred should have been populated inside the reservation. Let's abort now */
				goto fail;
		}
		else
		if(popmap_is_clear(rv->popmap, i))
		{
			/* prepopulate this page */
			/*
			 * requires vm_page_alloc to allocate a free page from a reservation
			 * The vm_page_alloc has been modified such that with VM_ALLOC_RESERVONLY,
			 * it must fail if it cannot allocate a page from a reservation
			 * If the reservation was broken by swapping, prepopulation will fail correctly
			 */
			m = vm_page_alloc(object, rv->pindex + i,
				VM_ALLOC_NORMAL | VM_ALLOC_ZERO | VM_ALLOC_RESERVONLY);

			/*
			 * If the allocation fails, the reservation is already broken,
			 * do not pre-zero it
			 */
			if(m == NULL)
				goto fail;
			/* release the object lock to do pre-zero */
			VM_OBJECT_WUNLOCK(object);
			/* check if page m is correctly allocated and xbusied */
			vm_page_assert_xbusied(m);
			/* m is xbusied after vm_page_alloc, so we can safely zero it */
			if((m->flags & PG_ZERO) == 0)
			{
				/*
				 * pages allocated in the reservation are not in freelist,
				 * but acquiring free page queue list lock can prevent a page fault zeroing this page
				 * So it is safe here to zero this page.
				 * we cannot unlock the page queue lock to free the page, because it is not removed
				 * from a reservation pool
				 */
				pmap_zero_page_idle(m);
				m->flags |= PG_ZERO;
				vm_page_zero_count ++;
				async_prezero++;
			}
			else
				async_skipzero ++;
			/* lock the vm object again once page zeroing is done */
			VM_OBJECT_WLOCK(object);

			/* the lock contention optimization of vm_object could end up with a NULL
			 * object that has been released when we zero the page.
			 * only validate and enqueue the page if the object is still alive
			 */
			if(m->object != NULL && m->object == rv->object)
			{
				/* validate, explicitly dirty and unbusy this page (copied from vm_page_grab_pages) */
				m->valid = VM_PAGE_BITS_ALL;
				/* put it in the active queue */
				vm_page_lock(m);
				vm_page_activate(m);
				vm_page_unlock(m);
				vm_page_xunbusy(m);
			}
			else
			{
				/* unbusy it anyway */
				vm_page_xunbusy(m);
				goto fail;
			}

		}
	}

	/*
	 * The prepopulation succeeds
	 * The reservation must get fully populated now
	 */
	VM_OBJECT_WUNLOCK(object);
	if(verbose && ((pop_succ % 10) == 0))
		printf("[prepopulate] succ|brok|fail|zero:[%d|%d|%d|%d]\n",
			pop_succ, pop_broken, pop_fail, async_prezero);
	pop_succ ++;
	return TRUE;

fail:
	VM_OBJECT_WUNLOCK(object);
	if(rv->object == NULL)
		pop_broken ++;
	else
	{
		rv->inpartpopq |= RV_BADBOY;
		pop_fail ++;
	}
	return FALSE;
}

/*
 * scan partpopq of reservations for async promotion
 * This function is serving for asyncpromo daemon, it is called
 * with vm_page_queue_free_mtx locked
 */
static void
vm_reserv_scan_partpopq(void)
{
	vm_reserv_t rv, prev;
	vm_object_t obj;
	int counter, eligible, pop_cnt;
	boolean_t queue_locked;

	/*  This code segment is hardcoded for 2MB superpages */
	counter = eligible = pop_cnt = 0;

	/*
	 *  The traversal uses an async_marker to help locate the position in partpopq
	 *  So that even though vm_page_queue_free_mtx is not always held,
	 *  it is safe to keep traversing the TAILQ
	 */
	mtx_lock(&vm_page_queue_free_mtx);
	for (rv = TAILQ_LAST(&vm_rvq_partpop, rvlist);
		 rv != NULL && pop_cnt < pop_budget;
		 rv = prev) {
		mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
		/* there are async and compact markers */
		if(rv->inpartpopq & RV_MARKER)
			continue;
		TAILQ_INSERT_BEFORE(rv, &async_marker, partpopq);
		mtx_unlock(&vm_page_queue_free_mtx);
		queue_locked = FALSE;

		counter ++;
		obj = rv->object;
		/*
		 * Find the reservation possible and eligible to be promoted.
		 * A deadbeef reservation can never be promoted,
		 * because there are other reservations
		 * transferred to the same object with the same pindex.
		 * None of them can be fully populated.
		 */
		if(obj != NULL && (obj->type == OBJT_DEFAULT || obj->type == OBJT_SWAP)
			&& obj->backing_object == NULL
			&& rv->inpartpopq == RV_INPARTPOPQ
			&& rv->popcnt >= pop_threshold)
		{
			/* rv will be removed from partpopq if gets prepopulated */
			pop_cnt += vm_reserv_prepopulate(rv);
		}

		if (!queue_locked) {
			mtx_lock(&vm_page_queue_free_mtx);
			queue_locked = TRUE;
		}
		mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
		prev = TAILQ_PREV(&async_marker, rvlist, partpopq);
		TAILQ_REMOVE(&vm_rvq_partpop, &async_marker, partpopq);
	}
	mtx_unlock(&vm_page_queue_free_mtx);

	return ;
}

/* inactive reservation threshold is 10s, default migration budget is 10MB/s */
static int inactive_thre = 10000, migrate_budget = 10 * 256;

SYSCTL_INT(_vm_reserv, OID_AUTO, inactive_thre, CTLFLAG_RWTUN,
    &inactive_thre, 0, "inactive timeout threshold");
SYSCTL_INT(_vm_reserv, OID_AUTO, migrate_budget, CTLFLAG_RWTUN,
    &migrate_budget, 0, "memory compaction budget");

static void
vm_reserv_evict_inactive(void)
{
	int work, exit;
	vm_reserv_t next, rv;
	vm_object_t obj;

	work = exit = 0;
	/*
	 *  The traversal uses evict_marker to help locate the position in partpopq
	 *  So that even though vm_page_queue_free_mtx is not always held,
	 *  it is safe to keep traversing the TAILQ
	 */
	mtx_lock(&vm_page_queue_free_mtx);
	for (rv = TAILQ_FIRST(&vm_rvq_partpop);
		 rv != NULL;
		 rv = next)
	{
		mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
		/* there are async and evict markers */
		if(rv->inpartpopq & RV_MARKER)
			continue;

		TAILQ_INSERT_AFTER(&vm_rvq_partpop, rv, &evict_marker, partpopq);
		mtx_unlock(&vm_page_queue_free_mtx);

		if(work < migrate_budget &&
			(ticks - rv->timestamp > inactive_thre
			|| need_migrate(rv->inpartpopq)))
		{
			obj = rv->object;
			if(obj != NULL && (obj->flags & (OBJ_FICTITIOUS | OBJ_UNMANAGED)) == 0)
			{
				/* let's evict this reservation */
				work += rv->popcnt;
				vm_page_reclaim_run(VM_ALLOC_NORMAL, 512, rv->pages, 0);
			}
		}
		else
			/* optimization because the list is temporally ordered */
			exit = 1;

		mtx_lock(&vm_page_queue_free_mtx);
		mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
		next = TAILQ_NEXT(&evict_marker, partpopq);
		TAILQ_REMOVE(&vm_rvq_partpop, &evict_marker, partpopq);
		if(exit)
			break;
	}
	mtx_unlock(&vm_page_queue_free_mtx);
	return ;
}

/*
 * TODO: A wakeup mechanism is required so that a page fault might
   kick off asyncpromo
 */
static void
vm_reserv_asyncpromo(void __unused *arg)
{
	vm_reserv_init_marker(&async_marker);
	vm_reserv_init_marker(&evict_marker);

	for (;;)
	{
		/* Are there reservations satisfying promotion condition ? */
		if (enable_prezero)
		{
			vm_reserv_scan_partpopq();
			/*
			 * tsleep does not unlock any mutex lock
			 * wakeup_frequency is tunable via sysctl, default = 0.1hz
			 * hz ~= 1s
			 */
		}

		/* evict inactive partpopq */
		if (enable_compact)
		{
			vm_reserv_evict_inactive();
		}

		/* Try to sleep no matter if enable_prezero */
		if(enable_sleep)
		{
			/* sleeps without holding any lock */
			tsleep(&async_promote, 0,
			    "asyncpromo", wakeup_frequency * hz / wakeup_time);
		}
	}
}

static void
asyncpromo_start(void __unused *arg)
{
	int error;
	struct proc *p;
	struct thread *td;

	error = kproc_create(vm_reserv_asyncpromo, NULL, &p, RFSTOPPED, 0,
		"asyncpromo");
	if (error)
		panic("asyncpromo_start: error %d\n", error);
	td = FIRST_THREAD_IN_PROC(p);
	thread_lock(td);

	/* We're an idle task, don't count us in the load. */
	td->td_flags |= TDF_NOLOAD;
	sched_class(td, PRI_IDLE);
	sched_prio(td, PRI_MAX_IDLE);
	sched_add(td, SRQ_BORING);
	thread_unlock(td);
}
SYSINIT(asyncpromo, SI_SUB_KTHREAD_VM, SI_ORDER_ANY, asyncpromo_start, NULL);



static int compact_method = 0;
SYSCTL_INT(_vm_reserv, OID_AUTO, compact_method, CTLFLAG_RWTUN,
    &compact_method, 0, "compaction method");

static void
vm_reserv_compact()
{
	vm_reserv_t rv, prev;
	vm_object_t obj;
	vm_page_t m_run;
	vm_paddr_t high;
	int available_order_9;

	vm_reserv_init_marker(&compact_marker);

	if(compact_method)
		uprintf("\nset high to the end of each reservation\n");
	else
		uprintf("\nset high to 0\n");

	available_order_9 = vm_phys_count_order_9();
	uprintf("available order-9 pages before compaction:\n%d\n", available_order_9);
	uprintf("2MB FMFI before compaction:\n%d / 100\n", 100 - 100 * available_order_9 * (1 << 9) / vm_cnt.v_free_count);
	/*
	 *  TODO: traverse the list forwards instead of backwards.
	 */
	mtx_lock(&vm_page_queue_free_mtx);
	for (rv = TAILQ_LAST(&vm_rvq_partpop, rvlist);
		 rv != NULL;
		 rv = prev) {
		mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
		/* there are async and compact markers */
		if(rv->inpartpopq & RV_MARKER)
			continue;
		TAILQ_INSERT_BEFORE(rv, &compact_marker, partpopq);
		mtx_unlock(&vm_page_queue_free_mtx);

		obj = rv->object;
		if((obj->flags & (OBJ_FICTITIOUS | OBJ_UNMANAGED)) == 0)
		{
			/* let's reclaim this reservation */
			m_run = rv->pages;
			if(compact_method)
				high = VM_PAGE_TO_PHYS(m_run) + NBPDR;
			else
				high = 0;

			vm_page_reclaim_run(VM_ALLOC_NORMAL, 512, m_run, high);
		}

		mtx_lock(&vm_page_queue_free_mtx);
		mtx_assert(&vm_page_queue_free_mtx, MA_OWNED);
		prev = TAILQ_PREV(&compact_marker, rvlist, partpopq);
		TAILQ_REMOVE(&vm_rvq_partpop, &compact_marker, partpopq);
	}
	mtx_unlock(&vm_page_queue_free_mtx);
	available_order_9 = vm_phys_count_order_9();
}

/*
 * a debug sysctl to relocate all partial reservations
 */
static int
debug_vm_reserv_compact(SYSCTL_HANDLER_ARGS)
{
	int error, i;

	i = 0;
	error = sysctl_handle_int(oidp, &i, 0, req);
	if (error)
		return (error);
	if (i != 0)
		vm_reserv_compact();
	return (0);
}

SYSCTL_PROC(_vm_reserv, OID_AUTO, compact, CTLTYPE_INT | CTLFLAG_RW, 0, 0,
    debug_vm_reserv_compact, "I", "compact all partial popq");


static void
vm_reserv_count_inactive()
{
	struct vm_domain *vmd;
	vm_page_t m, next;
	struct vm_pagequeue *pq;
	int maxscan;

	vmd = &vm_dom[0];
	int clean_cnt = 0;

	pq = &vmd->vmd_pagequeues[PQ_INACTIVE];
	maxscan = pq->pq_cnt;
	vm_pagequeue_lock(pq);
	for (m = TAILQ_FIRST(&pq->pq_pl);
	     m != NULL && maxscan-- > 0;
	     m = next) {

		next = TAILQ_NEXT(m, plinks.q);

		/*
		 * skip marker pages
		 */
		if (m->flags & PG_MARKER)
			continue;

		if (m->dirty != VM_PAGE_BITS_ALL && pmap_is_modified(m))
			clean_cnt ++;
	}
	vm_pagequeue_unlock(pq);

	uprintf("\nNumber of clean pages: %d\n", clean_cnt);
}

/*
 * a debug sysctl to relocate all partial reservations
 */
static int
debug_vm_reserv_count(SYSCTL_HANDLER_ARGS)
{
	int error, i;

	i = 0;
	error = sysctl_handle_int(oidp, &i, 0, req);
	if (error)
		return (error);
	if (i != 0)
		vm_reserv_count_inactive();
	return (0);
}

SYSCTL_PROC(_vm_reserv, OID_AUTO, count, CTLTYPE_INT | CTLFLAG_RW, 0, 0,
    debug_vm_reserv_count, "I", "count clean inactive pages");

#endif	/* VM_NRESERVLEVEL > 0 */
