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

struct addrspace *as_create(char *name) {
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}
	as->name = kstrdup(name);
	as->pages = NULL;
	as->regions = NULL;
	as->stack = NULL;
	as->heap = NULL;
	as->heap_end = (vaddr_t)0;
	as->heap_start = (vaddr_t)0;
	as->destroying = false;
	as->last_activation = (time_t)0;
	as->is_active = false;
	spinlock_init(&as->spinlock);

	return as;
}

static int as_num_pages(struct addrspace *as) {
	struct page_table_entry *pte = as->pages;
	int i = 0;
	int max_pages = coretotal();
	while (pte != NULL) {
		i++;
		if (i >= max_pages) {
			panic("cycle detected in as_num_pages");
		}
		pte = pte->next;
	}
	return i;
}

static time_t as_timestamp(void) {
	struct timespec tv;
	gettime(&tv);
	return tv.tv_sec;
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

	// Now actually allocate new pages for these regions, along with pages for stack and 1 initial heap page
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
		paddr_t paddr = alloc_upages(1);
		if (paddr == 0) {
			as_destroy(new);
			return ENOMEM;
		}
		new_heappage->paddr = paddr;
		new_heappage->vaddr = old_heappage->vaddr;
		new_heappage->page_entry_type = PAGE_ENTRY_TYPE_HEAP;
		new_heappage->permissions = old_heappage->permissions;
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

void as_destroy(struct addrspace *as) {
	DEBUGASSERT(as != NULL);
	if (as->destroying) { return; }
	spinlock_acquire(&as->spinlock);
	if (as->destroying) { return; }
	as->destroying = true;
	spinlock_release(&as->spinlock);
	as->is_active = false;

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
		free_upages(pte->paddr);
		pagetemp = pte;
		pte = pte->next;
		pagetemp->next = NULL;
		kfree(pagetemp);
	}
	spinlock_cleanup(&as->spinlock);
	kfree(as->name);
	kfree(as);
}

//static
//void
//as_zero_region(paddr_t paddr, unsigned npages)
//{
//	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
//}

void as_activate(void) {
	int i, spl;

	struct addrspace *as = proc_getas();
	if (!as) { return; }

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}
	splx(spl);

	as->is_active = true;
	as->last_activation = as_timestamp();
}

void as_deactivate(void) {
	struct addrspace *as = proc_getas();
	as->is_active = false;
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
// heap page
int as_prepare_load(struct addrspace *as) {
	paddr_t paddr;
	vaddr_t vaddr;

	DEBUGASSERT(as->pages == NULL);
	DEBUGASSERT(as->heap == NULL);
	DEBUGASSERT(as->stack == NULL);
	DEBUGASSERT(as->regions != NULL);

	// Setting up page tables
	struct regionlist *regionlst;
	struct page_table_entry *pages;
	regionlst = as->regions;
	size_t i;
	while (regionlst != NULL){
		vaddr = regionlst->vbase;
		for (i=0; i<regionlst->npages; i++) {
			if (as->pages==NULL) {
				as->pages = (struct page_table_entry *)kmalloc(sizeof(*pages));
				as->pages->vaddr = vaddr;
				as->pages->permissions = regionlst->permissions;
				as->pages->next = NULL;
				as->pages->page_entry_type = PAGE_ENTRY_TYPE_EXEC;
				paddr = alloc_upages(1);
				if (paddr == 0) {
					return ENOMEM;
				}
				as->pages->paddr = paddr;
			} else {
				for (pages=as->pages; pages->next!=NULL; pages=pages->next); // get last page
				pages->next = (struct page_table_entry *)kmalloc(sizeof(*pages));
				pages->next->vaddr = vaddr;
				pages->next->permissions = regionlst->permissions;
				pages->next->next = NULL;
				pages->next->page_entry_type = PAGE_ENTRY_TYPE_EXEC;
				paddr = alloc_upages(1);
				if (paddr == 0){
					return ENOMEM;
				}
				pages->next->paddr = paddr;
			}

			vaddr += PAGE_SIZE;
		}

		regionlst = regionlst->next;
	}

	// New Code
	vaddr_t stackvaddr = USERSTACK - VM_STACKPAGES * PAGE_SIZE;
	for (pages=as->pages;pages->next!=NULL;pages=pages->next);
	for (int i=0; i<VM_STACKPAGES; i++){
		struct page_table_entry *stack = (struct page_table_entry *)kmalloc(sizeof(*pages));
		stack->page_entry_type = PAGE_ENTRY_TYPE_STACK;
		stack->permissions = PF_R|PF_W;
		pages->next = stack;
		if (i==0) {
			as->stack = stack; // bottom of stack
		}
		stack->vaddr = stackvaddr;
		stack->next = NULL;
		paddr = alloc_upages(1);
		if (paddr == 0) {
			return ENOMEM;
		}
		stack->paddr = paddr;
		stackvaddr = stackvaddr + PAGE_SIZE;
		pages = pages->next;
	}

	// start with 1 heap page, can be added to with sbrk() user syscall
	struct page_table_entry *heap_page = (struct page_table_entry *)kmalloc(sizeof(*pages));
	pages->next = heap_page;
	heap_page->next = NULL;
	heap_page->page_entry_type = PAGE_ENTRY_TYPE_HEAP;

	paddr = alloc_upages(1);
	if (paddr == 0) {
		return ENOMEM;
	}

	heap_page->paddr = paddr;
	heap_page->vaddr = vaddr;

	as->heap_start = vaddr; // starts at end of loaded data region
	as->heap_end = vaddr + PAGE_SIZE;
	as->heap = heap_page; // first heap page

	KASSERT(as->heap_start != 0);
	KASSERT(as->heap_end != 0);

	return 0;
}

vaddr_t as_heapend(struct addrspace *as) {
	return as->heap_end;
}

static vaddr_t as_stackbottom(struct addrspace *as) {
	return as->stack->vaddr;
}

static void dealloc_page_entries(struct page_table_entry *pte) {
	struct page_table_entry *last;
	while (pte) {
		DEBUGASSERT(pte->paddr > 0);
		free_upages(pte->paddr);
		last = pte;
		pte = pte->next;
		kfree(last);
	}
}

int as_growheap(struct addrspace *as, size_t bytes) {
	size_t nbytes = ROUNDUP(bytes, PAGE_SIZE);
	size_t npages = nbytes / PAGE_SIZE;
	if (as->heap_end + nbytes >= as_stackbottom(as)) {
		return ENOMEM;
	}
	if (corefree() < npages) {
		return ENOMEM;
	}
	struct page_table_entry *first_heap_pte;
	struct page_table_entry *last = NULL;
	vaddr_t vaddr = as->heap_end;
	for (size_t i = 0; i < npages; i++) {
		struct page_table_entry *pte = kmalloc(sizeof(*pte));
		DEBUGASSERT(pte);
		if (i == 0) {
			first_heap_pte = pte; // to free allocated pages if we get an error
		}
		pte->vaddr = vaddr;
		paddr_t paddr = alloc_upages(1);
		if (paddr == 0) {
			goto nomem;
		}
		pte->paddr = paddr;
		pte->permissions = PF_R|PF_W;
		pte->page_entry_type = PAGE_ENTRY_TYPE_HEAP;
		pte->next = NULL; pte->last = NULL;
		if (last) {
			last->next = pte;
		}
		vaddr += PAGE_SIZE;
		last = pte;
	}
	as->heap_end = vaddr;
	struct page_table_entry *page;
	int old_num_pages = as_num_pages(as);
	// link heap pages into as->pages
	for (page = as->pages; page->next != NULL; page = page->next); // get last page
	page->next = first_heap_pte;
	DEBUGASSERT((old_num_pages + (int)npages) == as_num_pages(as));
	return 0;
	nomem: {
		dealloc_page_entries(first_heap_pte);
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
		pte->last_fault_access = 0;
		pte->swap_offset = 0;
		pte->num_swaps = 0;
		pte = pte->next;
	}
	return 0;
}

// static bool should_swap() {
// 	return true;
// }

const char *swapfilefmt = "swapfile%d.dat";

int pte_swapout(struct addrspace *as, struct page_table_entry *pte, bool zero_fill_mem) {
	(void)as;
	if (!vm_can_sleep()) return -1;
	int spl = spl0();
	int pid = (int)curproc->pid;
	off_t write_offset = pte->swap_offset;
	bool append_offset = false;
	int openflags = O_WRONLY|O_CREAT;
	if (pte->num_swaps == 0) {
		write_offset = 0;
		append_offset = true;
		openflags |= O_APPEND;
	}
	char swapfname[30];
	snprintf(swapfname, 30, swapfilefmt, pid);

	struct uio myuio;
	struct iovec iov;
	char buf[PAGE_SIZE];
	memcpy(buf, (void*)PADDR_TO_KVADDR(pte->paddr), PAGE_SIZE);
	struct vnode *node;
	int result = vfs_open(swapfname, openflags, 0644, &node);
	if (result != 0) {
		splx(spl);
		return result;
	}
	struct stat st;
	result = VOP_STAT(node, &st); // fills out stat struct
	if (result != 0) {
		splx(spl);
		return 0;
	}
	off_t prev_filesize = st.st_size;
	if (append_offset) {
		write_offset = prev_filesize;
	}
	DEBUG(DB_VM, "Swapping out page entry to file %s, offset: %lld\n", swapfname, write_offset);
	uio_kinit(&iov, &myuio, buf, PAGE_SIZE, write_offset, UIO_WRITE);
	result = VOP_WRITE(node, &myuio);
	if (result != 0) {
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
	splx(spl);
	return 0;
}

int pte_swapin(struct addrspace *as, struct page_table_entry *pte, struct page **into_page) {
	(void)as;
	(void)into_page;
	if (!vm_can_sleep()) return -1;
	int spl = spl0();
	int pid = (int)curproc->pid;
	off_t read_offset = pte->swap_offset;
	DEBUGASSERT(read_offset > 0);
	DEBUGASSERT(pte->num_swaps > 0);
	int openflags = O_RDONLY;
	char swapfname[30];
	snprintf(swapfname, 30, swapfilefmt, pid);
	DEBUG(DB_VM, "Swapping in page entry from file %s, offset: %lld\n", swapfname, read_offset);
	struct uio myuio;
	struct iovec iov;
	char buf[PAGE_SIZE];
	uio_kinit(&iov, &myuio, buf, PAGE_SIZE, read_offset, UIO_READ);
	struct vnode *node;
	int result = vfs_open(swapfname, openflags, 0644, &node);
	if (result != 0) {
		splx(spl);
		return result;
	}
	result = VOP_READ(node, &myuio);
	if (result != 0) {
		splx(spl);
		return result;
	}
	memcpy((void*)PADDR_TO_KVADDR(pte->paddr), buf, PAGE_SIZE);
	pte->is_swapped = false;
	pte->is_dirty = false;
	DEBUG(DB_VM, "Done swapping in\n");
	vfs_close(node);
	splx(spl);
	return 0;
}

void as_touch_pte(struct page_table_entry *pte) {
	pte->last_fault_access = as_timestamp();
}

int as_define_stack(struct addrspace *as, vaddr_t *stackptr) {
	(void)as;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}
