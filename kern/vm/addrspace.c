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

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <spl.h>
#include <spinlock.h>
#include <thread.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <elf.h>
#include <clock.h>
#include <vfs.h>
#include <vnode.h>
#include <kern/stat.h>
#include <kern/unistd.h>

static time_t as_timestamp(void) {
	struct timespec tv;
	gettime(&tv);
	return tv.tv_sec;
}

static unsigned long new_addrspace_id() {
	return (unsigned long)random();
}

struct addrspace *as_create(char *name) {
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}
	as->name = kstrdup(name);
	as->id = new_addrspace_id();
	as->pid = 0;
	as->pages = NULL;
	as->regions = NULL;
	as->stack = NULL;
	as->heap = NULL;
	as->mmaps = NULL;
	as->heap_end = (vaddr_t)0;
	as->heap_start = (vaddr_t)0;
	as->heap_brk = (vaddr_t)0;
  as->heap_top = (vaddr_t)(USERSTACK - (VM_STACKPAGES * PAGE_SIZE));
	as->destroying = false;
	as->last_activation = (time_t)0;
	as->is_active = false;
	as->running_cpu_idx = -1;
	as->no_heap_alloc = false;
	as->refcount = 1;
	spinlock_init(&as->spinlock);

	return as;
}

int as_num_pages(struct addrspace *as) {
	if (as->regions == NULL) { return 0; }
	struct page_table_entry *pte = as->pages;
	int i = 0;
	while (pte != NULL) {
		i++;
		pte = pte->next;
	}
	return i;
}

static int as_num_pages_of_type(struct addrspace *as, enum page_entry_type type) {
	struct page_table_entry *pte = as->pages;
	int i = 0;
	while (pte != NULL) {
		if (pte->page_entry_type == type) {
			i++;
		}
		pte = pte->next;
	}
	return i;
}

static int as_num_pages_excluding_type(struct addrspace *as, enum page_entry_type type) {
	struct page_table_entry *pte = as->pages;
	int i = 0;
	while (pte != NULL) {
		if (pte->page_entry_type != type) {
			i++;
		}
		pte = pte->next;
	}
	return i;
}

static bool pte_is_active(struct page_table_entry *pte) {
	return (pte->coremap_idx > 0 && !pte->is_swapped);
}

static int as_num_active_pages(struct addrspace *as) {
	if (as->regions == NULL) { return 0; }
	struct page_table_entry *pte = as->pages;
	int i = 0;
	while (pte != NULL) {
		if (pte_is_active(pte)) {
			i++;
		}
		pte = pte->next;
	}
	return i;
}

static int as_num_regions(struct addrspace *as) {
	struct regionlist *region = as->regions;
	int i = 0;
	while (region != NULL) {
		i++;
		region = region->next;
	}
	return i;
}

bool as_is_destroyed(struct addrspace *as) {
	return as->id == 0 || as->destroying || as->name == NULL;// || (void*)as->name == (void*)0xdeadbeef;
}

static bool as_is_debuggable(struct addrspace *as) {
	return (!as_is_destroyed(as)) && (as->regions != NULL) && (as->pages != NULL);
}

void as_debug(struct addrspace *as) {
	DEBUGASSERT(as != NULL);
	if (!as_is_debuggable(as)) { return; }
	DEBUG(DB_VM, "Addrspace Debug Info: id=%lu (%s)\n", as->id, as->name);
	DEBUG(DB_VM, "  regions: %d\n", as_num_regions(as));
	DEBUG(DB_VM, "  pages:   %d (%d active)\n",
		as_num_pages(as), as_num_active_pages(as)
	);
}

static struct page_table_entry *as_copy_pte(struct addrspace *old, struct addrspace *new, struct page_table_entry *pte) {
	KASSERT(pte);
	(void)old;
	struct page_table_entry *new_pte = kmalloc(sizeof(*new_pte));
	KASSERT(new_pte);
	memcpy(new_pte, pte, sizeof(struct page_table_entry));
	new_pte->as = new;
	new_pte->debug_name = new->name;
	new_pte->is_swapped = false;
	new_pte->num_swaps = 0;
	new_pte->swap_offset = 0;
	new_pte->is_dirty = true;
	new_pte->tlb_idx = -1;
	new_pte->page_age = 0;
	new_pte->cpu_idx = curcpu->c_number;
	new_pte->next = NULL;
	return new_pte;
}

static struct mmap_reg *as_copy_mmap_region(struct addrspace *old, struct addrspace *new, struct mmap_reg *reg) {
	KASSERT((reg->flags & MAP_SHARED) != 0);
	struct mmap_reg *new_reg = kmalloc(sizeof(*new_reg));
	KASSERT(new_reg);
	DEBUG(DB_SYSCALL, "Copying mmap region\n");
	memcpy(new_reg, reg, sizeof(struct mmap_reg));
	if (new_reg->backing_obj) {
		KASSERT((new_reg->flags & MAP_ANONYMOUS) == 0);
		VOP_INCREF(new_reg->backing_obj);
	}
	new_reg->next = NULL;
	new_reg->pids_sharing = NULL;
	struct page_table_entry *old_pte, *last, *first;
	last = NULL;
	for (old_pte = reg->ptes; old_pte != NULL; old_pte = old_pte->next) {
		struct page_table_entry *pte = as_copy_pte(old, new, old_pte);
		KASSERT(pte);
		if (last) {
			last->next = pte;
		} else {
			first = pte;
		}
		last = pte;
	}
	new_reg->ptes = first;
	return new_reg;
}

int as_copy(struct addrspace *old, struct addrspace **ret) {
	struct addrspace *new;

	new = as_create(old->name);
	if (new == NULL) {
		return ENOMEM;
	}

	// Setup all the regions
	struct regionlist *itr, *newitr, *tmp;

	itr = old->regions;

	while (itr != NULL) {
		if (new->regions == NULL) {
			new->regions = (struct regionlist *)kmalloc(sizeof(*itr));
			new->regions->next = NULL;
			newitr = new->regions;
		} else {
			for (tmp=new->regions; tmp->next!=NULL; tmp=tmp->next) {} // get last
			newitr = (struct regionlist *) kmalloc(sizeof(*itr));
			tmp->next = newitr;
		}

		newitr->vbase = itr->vbase;
		newitr->npages = itr->npages;
		newitr->permissions = itr->permissions;
		newitr->next = NULL;

		itr = itr->next;
	}

	// Now actually allocate new pages for these regions, along with pages for stack
	// and 1 initial heap page
	if (as_prepare_load(new) != 0) {
		as_destroy(new);
		return ENOMEM;
	}

	struct page_table_entry *old_heappage = old->heap->next;
	struct page_table_entry *new_heappage = NULL;
	struct page_table_entry *last_new_heappage = new->heap;

	while (old_heappage) {
		// shared mmapped region structures are copied below
		if (old_heappage && old_heappage->page_entry_type == PAGE_ENTRY_TYPE_MMAP) {
			old_heappage = old_heappage->next;
			continue;
		}
		new_heappage = kmalloc(sizeof(*new_heappage));
		if (!new_heappage) {
			as_destroy(new);
			return ENOMEM;
		}
		new_heappage->vaddr = old_heappage->vaddr;
		new_heappage->page_entry_type = PAGE_ENTRY_TYPE_HEAP;
		new_heappage->permissions = old_heappage->permissions;
		paddr_t paddr = find_upage_for_entry(new_heappage, VM_PIN_PAGE, true);
		if (paddr == 0) {
			as_destroy(new);
			return ENOMEM;
		}
		new_heappage->paddr = paddr;
		last_new_heappage->next = new_heappage;
		last_new_heappage = new_heappage;
		old_heappage = old_heappage->next;
	}

	KASSERT(
		as_num_pages_excluding_type(old, PAGE_ENTRY_TYPE_MMAP) ==
		as_num_pages_excluding_type(new, PAGE_ENTRY_TYPE_MMAP)
	);

	// Copy the data from old to new
	struct page_table_entry *iterate1 = old->pages;
	struct page_table_entry *iterate2 = new->pages;
	 // copy heap, stack and executable memory regions
	while (iterate1 != NULL && iterate2 != NULL) {
		if (iterate1->page_entry_type == PAGE_ENTRY_TYPE_MMAP) {
			iterate1 = iterate1->next;
			continue;
		}
		DEBUGASSERT(iterate1->page_entry_type == iterate2->page_entry_type);
		memcpy((void *)PADDR_TO_KVADDR(iterate2->paddr),
			(const void *)PADDR_TO_KVADDR(iterate1->paddr), PAGE_SIZE);
		iterate1 = iterate1->next;
		iterate2 = iterate2->next;
	}

	// copy over structures for shared (MAP_SHARED) memory regions
	struct mmap_reg *mmap_region;
	struct mmap_reg *new_region, *last_region;
	last_region = NULL;
	for (mmap_region = old->mmaps; mmap_region != NULL; mmap_region = mmap_region->next) {
		if (mmap_region->flags & MAP_PRIVATE) {
			continue;
		}
		DEBUGASSERT(mmap_region->flags & MAP_SHARED);
		new_region = as_copy_mmap_region(old, new, mmap_region);
		if (last_region) {
			last_region->next = new_region;
		} else {
			new->mmaps = new_region;
		}
		last_region = new_region;
		struct page_table_entry *last_page;
		// link mmapped pages into new->pages
		for (last_page = new->pages; last_page->next != NULL; last_page = last_page->next) {}
		last_page->next = new_region->ptes;
	}

	new->heap_start = old->heap_start;
	new->heap_end = old->heap_end;
	new->heap_brk = old->heap_brk;
	new->heap_top = old->heap_top; // FIXME: this could be different if there are private mmapped regions!

	*ret = new;
	return 0;
}

void as_destroy(struct addrspace *as) {
	DEBUGASSERT(as != NULL);
	as->refcount--;
	if (as->refcount > 0) {
		DEBUG(DB_VM, "Decrementing refcount of address space %s\n", as->name);
		return;
	}
	DEBUG(DB_VM, "Destroying address space %s\n", as->name);
	lock_pagetable();
	if (as->destroying) {
		unlock_pagetable();
		return;
	}
	as->destroying = true;
	as->is_active = false;
	as->running_cpu_idx = -1;

	struct regionlist *reglst = as->regions;
	struct regionlist *temp;
	while (reglst) {
		temp = reglst;
		reglst = reglst->next;
		temp->next = NULL;
		kfree(temp);
	}

	struct page_table_entry *pte = as->pages;
	struct page_table_entry *pagetemp;
	// free heap pages, data and code pages and stack pages
	while (pte) {
		// memory-mapped pages are freed below
		if (pte->page_entry_type == PAGE_ENTRY_TYPE_MMAP) {
			pte = pte->next;
			continue;
		}
		if (pte->coremap_idx > 0) {
			struct page *core = &coremap[pte->coremap_idx];
			//DEBUGASSERT(core->entry == pte);
			free_upages(pte->paddr, false);
			//DEBUGASSERT(pte->coremap_idx == 0);
			DEBUGASSERT(core->entry == NULL);
		}
		pagetemp = pte;
		pte = pte->next;
		kfree(pagetemp);
	}

	struct mmap_reg *mmap_region = as->mmaps;
	struct mmap_reg *cur_reg;
	while (mmap_region) {
		if ((mmap_region->flags & MAP_SHARED) && mmap_region->opened_by != as->pid) {
			pte = mmap_region->ptes;
			struct page_table_entry *next;
			for (unsigned i = 0; i < mmap_region->num_pages; i++) {
				next = pte->next;
				kfree(pte);
				pte = next;
			}
		} else {
			as_rm_mmap(as, mmap_region); // actually release the underlying physical pages
			pte = mmap_region->ptes;
			struct page_table_entry *next;
			for (unsigned i = 0; i < mmap_region->num_pages; i++) {
				next = pte->next;
				kfree(pte);
				pte = next;
			}
		}
		cur_reg = mmap_region;
		mmap_region = mmap_region->next;
		kfree(cur_reg);
	}

	spinlock_cleanup(&as->spinlock);
	kfree(as->name);
	kfree(as);
	unlock_pagetable();
}

void as_activate(void) {
	int i, spl;
	struct addrspace *as = proc_getas();
	spl = splhigh();
	/* Disable interrupts on this CPU while frobbing the TLB. */
	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	splx(spl);
	if (!as) { return; }
	as->is_active = true;
	as->running_cpu_idx = curcpu->c_number;
	as->last_activation = as_timestamp();
}

void as_deactivate(void) {
	struct addrspace *as = proc_getas();
	as->is_active = false;
	as->running_cpu_idx = -1;
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment.
 */
int as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		int readable, int writeable, int executable) {
	size_t npages;

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */

	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	struct regionlist *end;
	if (as->regions != NULL) {
		end = as->regions->last;
	}

	if (as->regions == NULL) {
		as->regions = (struct regionlist *)kmalloc(sizeof(*end));
		as->regions->next = NULL;
		as->regions->last = as->regions;
		end = as->regions;
	} else {
		end = as->regions->last;
		end->next = (struct regionlist *)kmalloc(sizeof(*end));
		end = end->next;
		end->next = NULL;
		as->regions->last = end;
	}

	end->vbase = vaddr;
	end->npages = npages;
	end->permissions = 7 & (readable | writeable | executable);
	DEBUG(DB_VM, "loading region with permissions: %d\n", end->permissions);

	return 0;
}

// Allocates new pages for code and data regions, along with pages for entire stack and 1 initial
// heap page. It still should work even if as->regions is NULL, because we also allocate pages
// for the kernel's kswapd process this way.
int as_prepare_load(struct addrspace *as) {
	paddr_t paddr;
	vaddr_t vaddr;

	DEBUGASSERT(as->pages == NULL);
	DEBUGASSERT(as->heap == NULL);
	DEBUGASSERT(as->stack == NULL);
	//DEBUGASSERT(as->regions != NULL);

	// Setting up page tables
	struct regionlist *regionlst;
	struct page_table_entry *pages;
	regionlst = as->regions;
	size_t i;
	while (regionlst != NULL){
		vaddr = regionlst->vbase;
		for (i=0; i<regionlst->npages; i++) {
			if (as->pages==NULL) {
				as->pages = kmalloc(sizeof(*pages));
				KASSERT(as->pages);
				memset(as->pages, 0, sizeof(*pages));
				as->pages->vaddr = vaddr;
				as->pages->as = as;
				as->pages->permissions = regionlst->permissions;
				as->pages->next = NULL;
				as->pages->page_entry_type = PAGE_ENTRY_TYPE_EXEC;
				paddr = find_upage_for_entry(as->pages, VM_PIN_PAGE, true);
				if (paddr == 0) { // TODO: swap out!
					return ENOMEM;
				}
				as->pages->paddr = paddr;
			} else {
				for (pages=as->pages; pages->next!=NULL; pages=pages->next) { } // get last page
				pages->next = kmalloc(sizeof(*pages));
				KASSERT(pages->next);
				memset(pages->next, 0, sizeof(*pages));
				pages->next->vaddr = vaddr;
				pages->next->as = as;
				pages->next->permissions = regionlst->permissions;
				pages->next->next = NULL;
				pages->next->page_entry_type = PAGE_ENTRY_TYPE_EXEC;
				paddr = find_upage_for_entry(pages->next, VM_PIN_PAGE, true);
				if (paddr == 0){
					return ENOMEM;
				}
				pages->next->paddr = paddr;
			}
			memset((void*)PADDR_TO_KVADDR(paddr), 0, PAGE_SIZE);
			vaddr += PAGE_SIZE;
		}

		regionlst = regionlst->next;
	}

	// Allocate one page for injected code. This is always the last executable page in as->pages
	// Right now this page is only used for injecting 12 bytes of assembly code for the return from signal
	// handler trampoline... so it's sort of a waste.
	// TODO: use unused space in executable pages, if available.
	if (as->pages) {
		struct page_table_entry *last_page = as->pages;
		for (last_page=as->pages; last_page->next!=NULL; last_page=last_page->next) {}
		DEBUGASSERT(last_page->page_entry_type == PAGE_ENTRY_TYPE_EXEC);
		struct page_table_entry *injected = kmalloc(sizeof(*injected));
		bzero(injected, sizeof(*injected));
		injected->vaddr = vaddr;
		injected->as = as;
		injected->permissions = PROT_READ|PROT_EXEC;
		injected->next = NULL;
		injected->page_entry_type = PAGE_ENTRY_TYPE_EXEC;
		paddr = find_upage_for_entry(injected, VM_PIN_PAGE, true);
		if (paddr == 0){
			return ENOMEM;
		}
		injected->paddr = paddr;
		injected->flags |= PAGE_ENTRY_FLAG_INJECTED;
		last_page->next = injected;
		vaddr += PAGE_SIZE;
	}

	vaddr_t stackvaddr = USERSTACK - VM_STACKPAGES * PAGE_SIZE;
	if (as->pages) {
		for (pages=as->pages; pages->next!=NULL; pages=pages->next) {}
	}
	for (int i=0; i<VM_STACKPAGES; i++) {
		struct page_table_entry *stack = kmalloc(sizeof(*stack));
		KASSERT(stack);
		memset(stack, 0, sizeof(*stack));
		stack->page_entry_type = PAGE_ENTRY_TYPE_STACK;
		stack->permissions = PROT_READ|PROT_WRITE;
		stack->as = as;
		if (pages) {
			pages->next = stack;
		}
		if (i==0) {
			as->stack = stack; // bottom of stack
			if (as->pages == NULL) {
				as->pages = stack;
			}
		}
		stack->vaddr = stackvaddr;
		stack->next = NULL;
		paddr = find_upage_for_entry(stack, VM_PIN_PAGE, true);
		if (paddr == 0) {
			return ENOMEM;
		}
		stack->paddr = paddr;
		stackvaddr = stackvaddr + PAGE_SIZE;
		pages = stack;
	}

	if (as->no_heap_alloc) {
		return 0;
	}

	// start with 1 heap page, can be added to with sbrk() user syscall
	struct page_table_entry *heap_page = kmalloc(sizeof(*heap_page));
	KASSERT(heap_page);
	memset(heap_page, 0, sizeof(*heap_page));
	pages->next = heap_page;
	heap_page->next = NULL;
	heap_page->page_entry_type = PAGE_ENTRY_TYPE_HEAP;
	heap_page->as = as;
	heap_page->permissions = PF_R|PF_W;
	heap_page->vaddr = vaddr;
	paddr = find_upage_for_entry(heap_page, VM_PIN_PAGE, true);
	if (paddr == 0) {
		return ENOMEM;
	}
	memset((void*)PADDR_TO_KVADDR(paddr), 0, PAGE_SIZE);
	heap_page->paddr = paddr;

	as->heap_start = vaddr; // starts at end of loaded data region
	as->heap_brk = vaddr;
	as->heap_end = vaddr + PAGE_SIZE;
	as->heap = heap_page; // first heap page

	KASSERT(as->heap_start != 0);
	KASSERT(as->heap_end != 0);
	KASSERT(as->heap_start % PAGE_SIZE == 0);
	KASSERT(as->heap_end % PAGE_SIZE == 0);

	return 0;
}

vaddr_t as_heapend(struct addrspace *as) {
	return as->heap_end;
}

// static vaddr_t as_stackbottom(struct addrspace *as) {
// 	return as->stack->vaddr;
// }

static vaddr_t as_heaptop(struct addrspace *as) {
	return as->heap_top;
}

static void dealloc_page_entries(struct page_table_entry *pte, bool dolock) {
	struct page_table_entry *last;
	if (dolock)
		lock_pagetable();
	while (pte) {
		DEBUGASSERT(pte->paddr > 0);
		free_upages(pte->paddr, false);
		last = pte;
		pte = pte->next;
		kfree(last);
	}
	if (dolock)
		unlock_pagetable();
}

static void as_pte_init(struct addrspace *as, struct page_table_entry *pte,
	paddr_t paddr, int prot, enum page_entry_type entry_type) {
	pte->as = as;
	pte->paddr = paddr;
	pte->permissions = prot;
	pte->page_entry_type = entry_type;
	pte->next = NULL;
	pte->tlb_idx = -1;
	pte->cpu_idx = curcpu->c_number;
	pte->is_dirty = true;
	pte->is_swapped = false;
	pte->debug_name = as->name;
	pte->num_swaps = 0;
}

int as_growheap(struct addrspace *as, size_t bytes) {
	size_t nbytes = ROUNDUP(bytes, PAGE_SIZE);
	size_t npages = nbytes / PAGE_SIZE;
	DEBUGASSERT(npages > 0);
	// We initally give a process 1 page of heap, so the first allocation is just bookkeeping for us
	if (as->heap_brk + bytes <= as->heap_end) {
		DEBUG(DB_VM, "Fake allocation of %d bytes\n", (int)bytes);
		as->heap_brk += bytes;
		return 0;
	}
	if (as->heap_end + nbytes >= as_heaptop(as)) {
		return ENOMEM;
	}
	lock_pagetable();
	if (corefree() < npages) { // FIXME: lazily allocate core only when used!
		unlock_pagetable();
		return ENOMEM;
	}
	struct page_table_entry *first_heap_pte = NULL;
	struct page_table_entry *last = NULL;
	vaddr_t old_heapbrk = as->heap_brk;
	vaddr_t vaddr = as->heap_end;
	DEBUGASSERT(vaddr > 0);
	for (size_t i = 0; i < npages; i++) {
		struct page_table_entry *pte = kmalloc(sizeof(*pte));
		DEBUGASSERT(pte);
		memset(pte, 0, sizeof(*pte));
		if (i == 0) {
			first_heap_pte = pte; // to free allocated pages if we get an error
		}
		pte->vaddr = vaddr;
		paddr_t paddr = find_upage_for_entry(pte, VM_PIN_PAGE, false);
		if (paddr == 0) {
			if (i > 0) kfree(pte);
			goto nomem;
		}
		as_pte_init(as, pte, paddr, PF_R|PF_W, PAGE_ENTRY_TYPE_HEAP);
		if (last) {
			last->next = pte;
		}
		vaddr += PAGE_SIZE;
		last = pte;
	}
	as->heap_end = vaddr;
	as->heap_brk = old_heapbrk + bytes;
	struct page_table_entry *page;
	int old_num_pages = as_num_pages(as);
	int old_num_heap_pages = as_num_pages_of_type(as, PAGE_ENTRY_TYPE_HEAP);
	// link heap pages into as->pages
	for (page = as->pages; page->next != NULL; page = page->next) {} // get last page
	page->next = first_heap_pte;
	DEBUGASSERT((old_num_pages + (int)npages) == as_num_pages(as));
	DEBUGASSERT((old_num_heap_pages + (int)npages) == as_num_pages_of_type(as, PAGE_ENTRY_TYPE_HEAP));
	unlock_pagetable();
	return 0;
	nomem: {
		if (first_heap_pte) {
			dealloc_page_entries(first_heap_pte, false);
		}
		unlock_pagetable();
		return ENOMEM;
	}
}

int as_add_mmap(struct addrspace *as, size_t nbytes, int prot,
  							int flags, int fd, off_t file_offset, vaddr_t *mmap_startaddr,
								int *errcode) {
	struct mmap_reg *reg = NULL;
	size_t npages;
	size_t nbytes_orig = nbytes;
	struct filedes *file_des = NULL;
	DEBUGASSERT(as != NULL);
	if (nbytes == 0) {
		*errcode = EINVAL;
		return -1;
	}
	if (fd < 3 && (flags & MAP_ANONYMOUS) == 0) {
		*errcode = EINVAL;
		return -1;
	}
	if (prot != PROT_NONE && (prot & PROT_READ) == 0 &&
													 (prot & PROT_WRITE) == 0 &&
													 (prot & PROT_EXEC) == 0) {
		*errcode = EINVAL;
		return -1;
	}
	// must be one of the following
	if ((flags & MAP_PRIVATE) == 0 && (flags & MAP_SHARED) == 0) {
		*errcode = EINVAL;
		return -1;
	}
	if (fd > 2) {
		if (!file_is_open(fd)) {
			*errcode = EBADF;
			return -1;
		}
		file_des = filetable_get(curproc, fd);
		DEBUGASSERT(file_des);
		if (filedes_is_device(file_des)) {
			// not yet supported
			*errcode = EBADF;
			return -1;
		}
		if (prot & PROT_READ && !filedes_is_readable(file_des)) {
			*errcode = EPERM;
			return -1;
		}
		if (prot & PROT_WRITE && !filedes_is_writable(file_des)) {
			*errcode = EPERM;
			return -1;
		}
	}
	nbytes = ROUNDUP(nbytes, PAGE_SIZE);
	npages = nbytes / PAGE_SIZE;
	DEBUGASSERT(npages > 0);
	lock_pagetable();
	// FIXME: only check corefree() if we're mapping a file, otherwise lazily allocate core
	if (corefree() < npages || (as->heap_end + nbytes) >= as_heaptop(as)) {
		unlock_pagetable();
		*errcode = ENOMEM;
		return -1;
	}
	reg = kmalloc(sizeof(*reg));
	KASSERT(reg);
	if (fd < 3 && (flags & MAP_ANONYMOUS) == 0) {
		unlock_pagetable();
		*errcode = EINVAL;
		return -1;
	}
	reg->prot_flags = prot;
	reg->flags = flags;
	reg->fd = fd;
	reg->backing_obj = NULL;
	reg->file_offset = file_offset;
	reg->next = NULL;
	vaddr_t reg_btm = as->heap_top - (npages * PAGE_SIZE);
	DEBUGASSERT(reg_btm % PAGE_SIZE == 0);
	vaddr_t reg_top = as->heap_top;
	reg->start_addr = reg_btm;
	reg->end_addr = reg_top;
	reg->valid_end_addr = reg_btm + nbytes_orig;
	reg->opened_by = curproc->pid;
	reg->num_pages = npages;
	vaddr_t pte_vaddr;
	struct page_table_entry *first_pte = NULL;
	struct page_table_entry *last = NULL;

	for (size_t i = 0; i < npages; i++) {
		pte_vaddr = reg_btm + (i * PAGE_SIZE);
		struct page_table_entry *pte = kmalloc(sizeof(*pte));
		DEBUGASSERT(pte);
		memset(pte, 0, sizeof(*pte));
		if (i == 0) {
			first_pte = pte; // to free allocated pages if we get an error
		}
		pte->vaddr = pte_vaddr;
		paddr_t paddr = find_upage_for_entry(pte, 0, false);
		if (paddr == 0) {
			if (i > 0) kfree(pte);
			goto nomem;
		}
		// zero out page if MAP_ANONYMOUS is given (no support for MAP_UNINITIALIZED)
		if (flags & MAP_ANONYMOUS) {
			memset((void*)PADDR_TO_KVADDR(paddr), 0, PAGE_SIZE);
		}
		as_pte_init(as, pte, paddr, prot, PAGE_ENTRY_TYPE_MMAP);

		if (last) {
			last->next = pte;
		}
		last = pte;
	}
	unlock_pagetable();

	if (fd > 0) {
		// initialize physical memory pages with contents of file. Since pages aren't
		// necessarily contiguous, we iterate over them and copy the proper bytes into the
		// pages. TODO: lazily initialize memory with contents from file, only when they
		// touch the memory or perform operations on it.
		off_t file_size = filedes_size(file_des, errcode);
		if (file_size == -1) {
			return -1; // errcode set above
		}
		off_t old_file_offset = file_des->offset;
		reg->backing_obj = file_des->node;
		VOP_INCREF(reg->backing_obj);

		size_t readamt, bytes_left_to_read;
		void *buf;
		bytes_left_to_read = MINVAL(nbytes_orig, (size_t)file_size);
		int bytes_read = 0;
		paddr_t paddr = 0;
		off_t fdread_offset = file_offset;
		struct page_table_entry *pte = NULL;
		for (unsigned i = 0; i < reg->num_pages; i++) {
			struct iovec iov;
			struct uio myuio;
			if (i == 0) {
				pte = first_pte;
			} else {
				pte = pte->next;
			}
			if (bytes_left_to_read >= PAGE_SIZE) {
				readamt = PAGE_SIZE;
			} else {
				readamt = bytes_left_to_read;
			}
			paddr = pte->paddr;
			DEBUGASSERT(paddr > 0);
			buf = (void*)PADDR_TO_KVADDR(paddr);
			if (readamt > 0) {
				uio_kinit(&iov, &myuio, buf, readamt, fdread_offset, UIO_READ);
				bytes_read = file_read(file_des, &myuio, errcode);
				if (bytes_read != (int)readamt) {
					DEBUG(DB_SYSCALL, "mmap with FD error during file_read: %d (%s)\n", *errcode, strerror(*errcode));
					file_des->offset = old_file_offset;
					return -1;
				}
				fdread_offset += bytes_read;
				bytes_left_to_read -= readamt;
			}
			// zero out remaining contents of page, if applicable
			if (readamt < PAGE_SIZE) {
				memset((void*)PADDR_TO_KVADDR(paddr+readamt), 0, PAGE_SIZE - readamt);
			}
		}
		file_des->offset = old_file_offset;
	}

	// link region into as->mmaps
	struct mmap_reg *last_reg;
	if (!as->mmaps) {
		as->mmaps = reg;
	} else {
		for (last_reg = as->mmaps; last_reg->next != NULL; last_reg = last_reg->next) {}
		last_reg->next = reg;
	}

	reg->ptes = first_pte;

	// link ptes into as->pages
	struct page_table_entry *last_pte = NULL;
	for (last_pte = as->pages; last_pte->next != NULL; last_pte = last_pte->next) {}
	last_pte->next = first_pte;

	as->heap_top = reg_btm;
	*mmap_startaddr = reg->start_addr;
	return 0;
	nomem: {
		if (reg) {
			kfree(reg);
		}
		if (first_pte) {
			dealloc_page_entries(first_pte, false);
		}
		unlock_pagetable();
		*errcode = ENOMEM;
		return -1;
	}
}

int as_sync_mmap(struct addrspace *as, struct mmap_reg *mmap, unsigned start_page /* 0-indexed */, size_t length, int flags, int *errcode) {
	(void)flags;
	(void)as;
	DEBUGASSERT(start_page < mmap->num_pages);
	if (!mmap->backing_obj) {
		*errcode = EBADF;
		return -1;
	}
	if (length == 0) {
		return 0;
	}
	vaddr_t start_addr = mmap->start_addr + (PAGE_SIZE * start_page);
	off_t file_offset = mmap->file_offset + (PAGE_SIZE * start_page);
	vaddr_t end_addr = start_addr + length;
	if (!vm_region_contains_other(mmap->start_addr, mmap->valid_end_addr, start_addr, end_addr)) {
		*errcode = EFAULT;
		return -1;
	}
	size_t bytes_to_write = length;
	size_t writeamt = 0;
	struct page_table_entry *pte = mmap->ptes;
	for (unsigned i = start_page; i > 0; i--) { pte = pte->next; }
	for (unsigned i = start_page; i < mmap->num_pages; i++) {
		DEBUGASSERT(pte);
		struct iovec iov;
		struct uio myuio;
		if (bytes_to_write > PAGE_SIZE) {
			writeamt = PAGE_SIZE;
		} else {
			writeamt = bytes_to_write;
		}
		void *buf = (void*)pte->vaddr;
		uio_uinit(&iov, &myuio, buf, writeamt, file_offset, UIO_WRITE);
		int res = VOP_WRITE(mmap->backing_obj, &myuio);
		if (res != 0) {
			*errcode = res;
			return -1;
		}
		bytes_to_write -= writeamt;
		if (bytes_to_write == 0) {
			break;
		}
		file_offset += writeamt;
		pte = pte->next;
	}
	return 0;
}

struct mmap_reg *as_mmap_for_region(struct addrspace *as, vaddr_t startaddr, vaddr_t endaddr) {
	KASSERT(startaddr % PAGE_SIZE == 0);
	KASSERT(startaddr < endaddr);
	struct mmap_reg *mmap;
	for (mmap = as->mmaps; mmap != NULL; mmap = mmap->next) {
		if (mmap->start_addr <= startaddr && mmap->valid_end_addr >= endaddr) {
			return mmap;
		}
	}
	return NULL;
}

// Adds a pidlink (pidlist entry) to the given mmap, for use when a process
// shares the mapping. Currently this gets run on proc_fork when the parent
// has shared mappings.
void mmap_add_shared_pid(struct mmap_reg *mmap, pid_t pid) {
	KASSERT(mmap->flags & MAP_SHARED);
	struct pidlist *pidlink = kmalloc(sizeof(*pidlink));
	KASSERT(pidlink);
	pidlink->pid = pid;
	pidlink->next = NULL;

	struct pidlist *curlink = mmap->pids_sharing;
	if (curlink) {
		while (curlink->next) { curlink = curlink->next; }
		curlink->next = pidlink;
	} else {
		mmap->pids_sharing = pidlink;
	}
}

// unlinks the page table entries from as->pages that belong to the mmap with start address start_addr
static int as_unlink_mmapped_pages(struct addrspace *as, vaddr_t start_addr, unsigned num_pages) {
	struct page_table_entry *prev, *cur, *next_after_reg;
	prev = NULL;
	cur = as->pages;
	while (cur) {
		if (cur->page_entry_type == PAGE_ENTRY_TYPE_MMAP && cur->vaddr == start_addr) {
			next_after_reg = cur;
			while (num_pages > 0) {
				next_after_reg = next_after_reg->next;
				num_pages--;
			}
			DEBUGASSERT(prev);
			prev->next = next_after_reg;
			return 0;
		}
		prev = cur;
		cur = cur->next;
	}
	return -1;
}

// frees the mmap and the page table entries, and unlinks the mmap_reg from as->mmaps,
// as well as the ptes from as->pages
int as_free_mmap(struct addrspace *as, struct mmap_reg *reg) {
	struct mmap_reg *cur = as->mmaps;
	struct mmap_reg *prev = as->mmaps;
	// unlink mmap from as->mmaps
	if (cur == reg) {
		as->mmaps = cur->next;
	} else {
		while (prev->next != reg) {
			prev = prev->next;
		}
		KASSERT(prev->next == reg);
		prev->next = reg->next;
	}
	unsigned old_num_pages = (unsigned)as_num_pages(as);
	KASSERT(as_unlink_mmapped_pages(as, reg->start_addr, reg->num_pages) == 0);
	unsigned new_num_pages = (unsigned)as_num_pages(as);
	DEBUGASSERT(new_num_pages == old_num_pages - reg->num_pages);
	struct page_table_entry *pte = reg->ptes;
	struct page_table_entry *next;
	for (unsigned i = 0; i < reg->num_pages; i++) {
		next = pte->next;
		kfree(pte);
		pte = next;
	}
	if (reg->backing_obj) {
		VOP_DECREF(reg->backing_obj);
	}

	kfree(reg);
	return 0;
}

// release the physical pages from the mmapped region, and remove the region and all page references
// from the processes that share this region, if applicable. NOTE: pagetable should be locked.
int as_rm_mmap(struct addrspace *as, struct mmap_reg *reg) {
	struct page_table_entry *pte = reg->ptes;
	if (reg->flags & MAP_SHARED) {
		DEBUGASSERT(reg->opened_by == as->pid);
		DEBUG(DB_SYSCALL, "Removing shared mapping from as %s\n", as->name);
	}
	for (unsigned i = 0; i < reg->num_pages; i++) {
		DEBUGASSERT(pte);
		DEBUGASSERT(pte->page_entry_type == PAGE_ENTRY_TYPE_MMAP);
		if (pte->coremap_idx > 0) {
			struct page *core = &coremap[pte->coremap_idx];
			DEBUG(DB_SYSCALL, "Removing physical page for shared mapping\n");
			free_upages(pte->paddr, false);
			DEBUGASSERT(core->entry == NULL);
			memset((void*)PADDR_TO_KVADDR(pte->paddr), 0, PAGE_SIZE);
		}
		pte = pte->next;
	}

	if (reg->flags & MAP_SHARED) {
		struct pidlist *pidlink = reg->pids_sharing;
		while (pidlink) {
			pid_t pid = pidlink->pid;
			struct proc *p = proc_lookup(pid);
			if (p) {
				struct addrspace *child_as = p->p_addrspace;
				struct mmap_reg *child_mmap = child_as->mmaps;
				while (child_mmap && child_mmap->start_addr != reg->start_addr) {
					child_mmap = child_mmap->next;
				}
				if (child_mmap) {
					as_free_mmap(p->p_addrspace, child_mmap);
				}
			}
			pidlink = pidlink->next;
		}
	}
	return 0;
}

bool pte_can_handle_fault_type(struct page_table_entry *pte, int faulttype) {
	switch (faulttype) {
	case VM_FAULT_READONLY:
		return (pte->permissions & PF_W) != 0;
	case VM_FAULT_READ:
		return (pte->permissions & PF_R) != 0;
	case VM_FAULT_WRITE:
		return (pte->permissions & PF_W) != 0;
	default:
		panic("shouldn't get here");
		return false;
	}
}

static int as_inject_code(struct addrspace *as) {
	void *fn_ptr = sigcode;
	struct page_table_entry *pte = as->pages;
	while ((pte->flags & PAGE_ENTRY_FLAG_INJECTED) == 0) { pte = pte->next; }
	if (!pte) { return -1; }
	DEBUGASSERT(pte->page_entry_type == PAGE_ENTRY_TYPE_EXEC);
	vaddr_t vaddr = pte->vaddr;
	paddr_t paddr = pte->paddr;
	DEBUGASSERT(paddr > 0);
	// copy instructions from user signal handler return assembly code to address space of user,
	// so we can jump to it after we call the user's signal handler to return control to the OS.
	memcpy((void*)PADDR_TO_KVADDR(paddr), (const void*)fn_ptr, SIGRETCODE_BYTESIZE);
	as->sigretcode = vaddr;
	return 0;
}

int as_complete_load(struct addrspace *as) {
	as_inject_code(as);
	struct page_table_entry *pte = as->pages;
	while (pte) {
		pte->is_dirty = true;
		pte->is_swapped = false;
		pte->swap_offset = 0;
		pte->num_swaps = 0;
		pte->page_age = 0;;
		pte->as = as;
		pte->debug_name = as->name; // shares same mem as address space
		if (pte->coremap_idx > 0 && pte->page_entry_type != PAGE_ENTRY_TYPE_STACK) {
			// entries are pinned in as_prepare_load so as not to be swapped out before the process starts
			vm_unpin_page_entry(pte);
		}
		pte->tlb_idx = -1; // set during vm_fault, and used for doing TLB shootdowns and tlb_invalidation when we swap pages
		pte->cpu_idx = curcpu->c_number; // same as above
		pte = pte->next;
	}
	return 0;
}

// static bool should_swap() {
// 	return true;
// }

int as_swapout(struct addrspace *as) {
	if (!vm_can_sleep()) return -1;
	struct page_table_entry *pte = as->pages;
	while (pte) {
		pte_swapout(as, pte, true);
		pte = pte->next;
	}
	return 0;
}

int as_swapin(struct addrspace *as) {
	if (!vm_can_sleep()) return -1;
	struct page_table_entry *pte = as->pages;
	while (pte) {
		pte_swapin(as, pte, NULL);
		pte = pte->next;
	}
	return 0;
}

const char *swapfilefmt = "swapfile%d-%lu.dat";

int pte_swapout(struct addrspace *as, struct page_table_entry *pte, bool zero_fill_mem) {
	if (pte->is_swapped) {
		DEBUG(DB_VM, "Page entry already swapped\n");
		return -1;
	}
	if (!pte->is_dirty) {
		DEBUGASSERT(pte->num_swaps > 0);
		if (zero_fill_mem) {
			memset((void*)PADDR_TO_KVADDR(pte->paddr), 0, PAGE_SIZE);
		}
		pte->is_swapped = true;
		return 0;
	}
	if (!vm_can_sleep()) return -1;
	int spl = spl0();
	int pid = (int)as->pid;
	off_t write_offset = pte->swap_offset;
	bool append_offset = false;
	int openflags = O_WRONLY|O_CREAT;
	if (pte->num_swaps == 0) {
		write_offset = 0;
		append_offset = true;
	}
	char swapfname[30];
	bzero(swapfname, 30);
	snprintf(swapfname, 30, swapfilefmt, pid, as->id);
	char *swapfnamecopy = kstrdup(swapfname); // vfs_open munges our data!

	struct uio myuio;
	struct iovec iov;
	char *buf = kmalloc(PAGE_SIZE); // NOTE: can't use stack for some reason, need to figure out why
	memcpy(buf, (void*)PADDR_TO_KVADDR(pte->paddr), PAGE_SIZE);
	struct vnode *node;
	DEBUG(DB_VM, "Opening file for swap: %s\n", swapfname);
	int result = vfs_open(swapfnamecopy, openflags, 0644, &node);
	if (result != 0) {
		DEBUG(DB_VM, "Failed to open file for swap\n");
		kfree(swapfnamecopy);
		kfree(buf);
		splx(spl);
		return result;
	}

	if (append_offset) {
		struct stat st;
		result = VOP_STAT(node, &st); // fills out stat struct
		if (result != 0) {
			DEBUG(DB_VM, "Failed to stat file for swap\n");
			kfree(swapfnamecopy);
			kfree(buf);
			splx(spl);
			return result;
		}
		off_t prev_filesize = st.st_size;
		if (append_offset) {
			write_offset = prev_filesize;
		}
	}

	DEBUG(DB_VM, "Swapping out page entry (%lu) to file %s, offset: %lld\n",
		pte->coremap_idx, swapfname, write_offset
	);
	uio_kinit(&iov, &myuio, buf, PAGE_SIZE, write_offset, UIO_WRITE);
	result = VOP_WRITE(node, &myuio);
	if (result != 0) {
		DEBUG(DB_VM, "Failed to write file for swap\n");
		kfree(swapfnamecopy);
		kfree(buf);
		splx(spl);
		return result;
	}
	if (zero_fill_mem) {
		memset((void*)PADDR_TO_KVADDR(pte->paddr), 0, PAGE_SIZE);
	}
	pte->is_swapped = true;
	pte->swap_offset = write_offset;
	pte->is_dirty = false;
	pte->num_swaps++;
	DEBUG(DB_VM, "Done swapping out\n");
	vfs_close(node);
	kfree(swapfnamecopy);
	kfree(buf);
	splx(spl);
	return 0;
}

int pte_swapin(struct addrspace *as, struct page_table_entry *pte, struct page **into_page) {
	(void)into_page;
	if (!vm_can_sleep()) return -1;
	if (!pte->is_swapped) { return -1; }
	int spl = spl0();
	int pid = (int)as->pid;
	off_t read_offset = pte->swap_offset;
	DEBUGASSERT(read_offset >= 0);
	DEBUGASSERT(pte->num_swaps > 0);
	int openflags = O_RDONLY;
	char swapfname[30];
	bzero(swapfname, 30);
	snprintf(swapfname, 30, swapfilefmt, pid, as->id);
	char *swapfnamecopy = kstrdup(swapfname);
	struct uio myuio;
	struct iovec iov;
	char *buf = kmalloc(PAGE_SIZE);
	uio_kinit(&iov, &myuio, buf, PAGE_SIZE, read_offset, UIO_READ);
	struct vnode *node;
	int result = vfs_open(swapfnamecopy, openflags, 0644, &node);
	if (result != 0) {
		kfree(buf);
		kfree(swapfnamecopy);
		splx(spl);
		return result;
	}
	result = VOP_READ(node, &myuio);
	if (result != 0) {
		kfree(buf);
		kfree(swapfnamecopy);
		splx(spl);
		return result;
	}
	// get page
	paddr_t paddr = find_upage_for_entry(pte, 0, true);
	if (paddr == 0) {
		kfree(buf);
		kfree(swapfnamecopy);
		splx(spl);
		return ENOMEM;
	}
	DEBUG(DB_VM, "Swapping in page entry from file %s to page %lu, offset: %lld\n",
		swapfname, pte->coremap_idx, read_offset
	);
	if (into_page != NULL) {
		*into_page = &coremap[pte->coremap_idx];
	}
	pte->paddr = paddr;
	memcpy((void*)PADDR_TO_KVADDR(pte->paddr), buf, PAGE_SIZE);
	pte->is_swapped = false;
	pte->is_dirty = false;
	pte->page_age = 0;
	DEBUG(DB_VM, "Done swapping in\n");
	vfs_close(node);
	kfree(buf);
	kfree(swapfnamecopy);
	splx(spl);
	return 0;
}

void as_touch_pte(struct page_table_entry *pte) {
	//pte->last_fault_access = as_timestamp();
	(void)pte;
}

// is the given virtual address region within the allocated heap of the given address space?
bool as_heap_region_exists(struct addrspace *as, vaddr_t reg_btm, vaddr_t reg_top) {
	KASSERT(reg_btm < reg_top);
	KASSERT(as != NULL);
	struct page_table_entry *heap = as->heap; // bottom of heap
	if (!heap) {
		return false;
	}
	return reg_btm >= heap->vaddr && reg_top < as->heap_brk;
}
