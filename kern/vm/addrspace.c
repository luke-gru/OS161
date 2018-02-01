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

static struct spinlock addrspace_g_lock = SPINLOCK_INITIALIZER;

static unsigned long alloc_addrspace_id() {
	unsigned long as_id;
	spinlock_acquire(&addrspace_g_lock);
	as_id = ++last_addrspace_id;
	spinlock_release(&addrspace_g_lock);
	return as_id;
}

struct addrspace *as_create(char *name) {
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}
	as->name = kstrdup(name);
	as->id = alloc_addrspace_id();
	as->pid = 0;
	as->pages = NULL;
	as->regions = NULL;
	as->stack = NULL;
	as->heap = NULL;
	as->heap_end = (vaddr_t)0;
	as->heap_start = (vaddr_t)0;
	as->heap_brk = (vaddr_t)0;
	as->destroying = false;
	as->last_activation = (time_t)0;
	as->is_active = false;
	as->running_cpu_idx = -1;
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

static time_t as_timestamp(void) {
	struct timespec tv;
	gettime(&tv);
	return tv.tv_sec;
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
			for (tmp=new->regions; tmp->next!=NULL; tmp=tmp->next); // get last
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

	KASSERT(as_num_pages(old) == as_num_pages(new));

	// Copy the data from old to new
	struct page_table_entry *iterate1 = old->pages;
	struct page_table_entry *iterate2 = new->pages;

	while (iterate1 != NULL && iterate2 != NULL) {
		DEBUGASSERT(iterate1->page_entry_type == iterate2->page_entry_type);
		if (iterate1->page_entry_type == PAGE_ENTRY_TYPE_STACK) {
			memset((void *)PADDR_TO_KVADDR(iterate2->paddr), 0, PAGE_SIZE); // zero out new stack memory
		} else {
			memcpy((void *)PADDR_TO_KVADDR(iterate2->paddr),
					(const void *)PADDR_TO_KVADDR(iterate1->paddr), PAGE_SIZE); // copy heap and executable memory regions
		}


		iterate1 = iterate1->next;
		iterate2 = iterate2->next;
	}

	*ret = new;
	return 0;
}

void as_lock_all(void) {
	spinlock_acquire(&addrspace_g_lock);
}

void as_unlock_all(void) {
	spinlock_release(&addrspace_g_lock);
}

void as_destroy(struct addrspace *as) {
	DEBUGASSERT(as != NULL);
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
	while (pte) {
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

			vaddr += PAGE_SIZE;
		}

		regionlst = regionlst->next;
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
		stack->permissions = PF_R|PF_W;
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

static vaddr_t as_stackbottom(struct addrspace *as) {
	return as->stack->vaddr;
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
	lock_pagetable();
	if (as->heap_end + nbytes >= as_stackbottom(as)) {
		return ENOMEM;
	}
	if (corefree() < npages) {
		return ENOMEM;
	}
	struct page_table_entry *first_heap_pte;
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
			goto nomem;
		}
		pte->as = as;
		pte->paddr = paddr;
		pte->permissions = PF_R|PF_W;
		pte->page_entry_type = PAGE_ENTRY_TYPE_HEAP;
		pte->next = NULL;
		pte->tlb_idx = -1;
		pte->cpu_idx = curcpu->c_number;
		pte->is_dirty = true;
		pte->is_swapped = false;
		pte->debug_name = as->name;
		pte->num_swaps = 0;
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
		dealloc_page_entries(first_heap_pte, false);
		unlock_pagetable();
		return ENOMEM;
	}
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

int as_complete_load(struct addrspace *as) {
	/*
	 * TODO: zero out stack?
	 */

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

const char *swapfilefmt = "swapfile%d-%d.dat";

int pte_swapout(struct addrspace *as, struct page_table_entry *pte, bool zero_fill_mem) {
	if (!vm_can_sleep()) return -1;
	if (pte->is_swapped) {
		DEBUG(DB_VM, "Page entry already swapped\n");
		return 0;
	}
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

	DEBUG(DB_VM, "Swapping out page entry to file %s, offset: %lld\n", swapfname, write_offset);
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
	if (!pte->is_swapped) { return 0; }
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
	DEBUG(DB_VM, "Swapping in page entry from file %s, offset: %lld\n", swapfname, read_offset);
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
	memcpy((void*)PADDR_TO_KVADDR(pte->paddr), buf, PAGE_SIZE);
	pte->is_swapped = false;
	pte->is_dirty = false;
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

int as_define_stack(struct addrspace *as, vaddr_t *stackptr) {
	(void)as;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}
