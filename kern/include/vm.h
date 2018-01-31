/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _VM_H_
#define _VM_H_

/*
 * VM system-related definitions.
 *
 * You'll probably want to add stuff here.
 */

struct addrspace;

#include <machine/vm.h>
#include <cpu.h>

/* Fault-type arguments to vm_fault() */
#define VM_FAULT_READ        0    /* A read was attempted */
#define VM_FAULT_WRITE       1    /* A write was attempted */
#define VM_FAULT_READONLY    2    /* A write to a readonly page was attempted*/

#define VM_STACKPAGES    12

#define VM_PIN_PAGE 1
#define VM_PAGE_AGE_MAX 100

enum page_t {
  PAGETYPE_KERN = 1,
  PAGETYPE_USER
};

enum pagestate_t {
  PAGESTATE_FREE = 0,
  PAGESTATE_INUSE
};

/* Initialization function */
void vm_bootstrap(void);
void kswapd_bootstrap(void);
void kswapd_start(void *data1, unsigned long data2);
paddr_t getppages(unsigned long npages, enum page_t pagetype, unsigned long *coremap_idx, bool dolock);
// returns number of free physical memory pages
unsigned long corefree(void);
// returns total number of physical memory pages
unsigned long coretotal(void);
void lock_pagetable(void);
void unlock_pagetable(void);
/*
 * Return amount of memory (in bytes) used by allocated coremap pages. If
 * there are ongoing allocations, this value could change after it is returned
 * to the caller. But it should have been correct at some point in time.
 */
unsigned long coremap_used_bytes(void);

struct page {
    vaddr_t va;
    paddr_t pa;
    struct page_table_entry *entry;
    int partofpage; // is used for de-allocation of blocks of pages
    bool contained;
    enum pagestate_t state;
    bool is_kern_page;
    // some RAM needs to be pinned, which means it isn't swappable. For instance,
    // if we're reading from a userptr buffer in one process, and get context switched,
    // we can't evict that userptr buffer page.
    bool is_pinned;
};

struct page *coremap;

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress);

/* Allocate/free kernel heap pages (called by kmalloc/kfree) */
vaddr_t alloc_kpages(int npages);
paddr_t alloc_upages(int npages, unsigned long *coremap_idx, bool dolock);
paddr_t find_upage_for_entry(struct page_table_entry *pte, int page_flags);
void free_kpages(vaddr_t addr);
void free_upages(paddr_t addr, bool dolock);



/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown(const struct tlbshootdown *);
void vm_tlbshootdown_all(void);
bool vm_can_sleep(void);
void vm_unpin_page_entry(struct page_table_entry *entry);


#endif /* _VM_H_ */
