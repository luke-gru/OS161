#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <vm.h>
#include <spl.h>
#include <spinlock.h>
#include <thread.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <clock.h>
#include <proc.h>
#include <cpu.h>
#include <limits.h>
#include <kern/unistd.h>

static bool beforeVM = true;
static unsigned long pages_in_coremap;
static int coremap_size;

paddr_t lastaddr, firstaddr, freeaddr;
struct page *coremap;

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

// number of free physical pages
unsigned long corefree(void) {
	int k=0;
	for (unsigned long i=0; i<pages_in_coremap; i++){
		if (coremap[i].state == PAGESTATE_FREE) {
			k++;
		}
	}
	return k;
}

unsigned long coremap_used_bytes(void) {
  return (pages_in_coremap - corefree()) * PAGE_SIZE;
}

// number of total physical pages
unsigned long coretotal(void) {
  return pages_in_coremap;
}

void lock_pagetable() {
	spinlock_acquire(&stealmem_lock);
}

void unlock_pagetable() {
  spinlock_release(&stealmem_lock);
}

void vm_bootstrap(void) {
	int page_num;
	paddr_t coremapaddr, temp;

	ram_getsize(&firstaddr, &lastaddr);
	lock_pagetable();

	page_num = (lastaddr-firstaddr) / PAGE_SIZE;

	freeaddr = firstaddr + page_num * sizeof(struct page);
	freeaddr = ROUNDUP(freeaddr, PAGE_SIZE);// added for pr->nfree error

	coremap = (struct page*)PADDR_TO_KVADDR(firstaddr);
	coremapaddr = freeaddr - firstaddr;
	coremap_size = ROUNDUP(coremapaddr, PAGE_SIZE) / PAGE_SIZE;

	pages_in_coremap = page_num;


	for (int i=0; i<page_num; i++) {

		if (i<coremap_size) {
      coremap[i].is_kern_page = true;
			coremap[i].state = PAGESTATE_INUSE;
		} else {
			coremap[i].state = PAGESTATE_FREE;
			coremap[i].contained = false;
      coremap[i].is_kern_page = false;
		}

		temp = PAGE_SIZE * i + freeaddr;
		coremap[i].pa = temp;
		coremap[i].va = PADDR_TO_KVADDR(temp);
    coremap[i].entry = NULL;
    coremap[i].is_pinned = false;
	}

	beforeVM = false;
	unlock_pagetable();
}

void kswapd_bootstrap() {
  struct proc *kswapd_proc = proc_create("kswapd", PROC_CREATEFL_NORM|PROC_CREATEFL_EMPTY_FT|PROC_CREATEFL_EMPTY_ENV);
  kswapproc = kswapd_proc;
  kswapd_proc->pid = KSWAPD_PID;
  kswapd_proc->p_parent = kproc;
  kswapd_proc->p_cwd = kproc->p_cwd;
	return; // NOTE: not running right now
  struct addrspace *as = as_create(kswapd_proc->p_name);
  KASSERT(as);
	return;
	as->no_heap_alloc = true;
  kswapd_proc->p_addrspace = as;
  KASSERT(as_prepare_load(as) == 0); // allocate userspace stack pages so we can jump to entrypoint func
  struct page_table_entry *pte = as->pages;
  char *pte_name = as->name;
	lock_pagetable();
  while (pte != NULL) {
    pte->debug_name = pte_name;
    KASSERT(pte->coremap_idx > 0);
    KASSERT(coremap[pte->coremap_idx].entry == pte);
    coremap[pte->coremap_idx].is_kern_page = true;
    coremap[pte->coremap_idx].is_pinned = true;
    pte = pte->next;
  }
	unlock_pagetable();
	struct cpu *non_boot_cpu = thread_get_cpu(3);
  int fork_res = thread_fork_in_cpu("kswapd", kswapd_proc, non_boot_cpu, kswapd_start, NULL, 0);
	KASSERT(fork_res == 0);
}

static void kswapd_age_pages() {
  int i;
  for (i = coremap_size; i < (int)pages_in_coremap; i++) {
    if (coremap[i].is_kern_page || coremap[i].state == PAGESTATE_FREE) {
      continue;
    }
    KASSERT(coremap[i].entry != NULL);
    KASSERT(coremap[i].entry->coremap_idx > 0);
    // this happens when proc_destroy doesn't call as_destroy(), just as_deactivate()
    // if (!coremap[i].entry->as->is_active) {
    //   coremap[i].entry->coremap_idx = 0;
    //   coremap[i].entry = NULL;
    //   coremap[i].state = PAGESTATE_FREE;
    //   continue;
    // }
    if (coremap[i].entry->page_age < VM_PAGE_AGE_MAX) {
      char *name = coremap[i].entry->debug_name;
      DEBUG(DB_VM, "\naging page %s: %d\n", name, (int)coremap[i].entry->page_age);
      coremap[i].entry->page_age++;
      //as_debug(coremap[i].entry->as);
    }
  }
}

static void kswapd_swapout_pages() {
  int i;
  for (i = coremap_size; i < (int)pages_in_coremap; i++) {
    if (coremap[i].is_kern_page || coremap[i].state == PAGESTATE_FREE) {
      continue;
    }
    if (coremap[i].is_pinned) {
      continue;
    }
    if (coremap[i].entry->page_entry_type == PAGE_ENTRY_TYPE_HEAP) {
      struct page_table_entry *pte = coremap[i].entry;
      char *name = pte->debug_name;
      if (pte->is_swapped || pte->page_age < 3) {
        continue;
      }
      DEBUG(DB_VM, "swapping out pages for %s\n", name);
      int swap_res;
      swap_res = pte_swapout(pte->as, pte, false);
      if (swap_res != 0) {
				panic("swap error");
				DEBUG(DB_VM, "Swap error: %d\n", swap_res);
        continue;
      }
      // NOTE: only invalidate the TLB if the process using this address space is currently running
      if (pte->as->running_cpu_idx >= 0) {
        if (pte->tlb_idx >= 0 && pte->tlb_idx < NUM_TLB) {
					// Is this CPU currently running the user process? Right now this can't happen,
					// as kswapd is running as a "virtual" user process, and it doesn't swap its own page
					// tables. In the future if we decide kswapd should be a kernel level thread without a
					// process attached, we could reach here.
          if (pte->cpu_idx == (short)curcpu->c_number) {
            DEBUG(DB_VM, "Invalidating page entry in curcpu's TLB, zeroing memory\n");
            tlb_write(TLBHI_INVALID(pte->tlb_idx), TLBLO_INVALID(), pte->tlb_idx);
            memset((void*)PADDR_TO_KVADDR(pte->paddr), 0, PAGE_SIZE);
          } else { // send a IPI to the right CPU.
						KASSERT(pte->cpu_idx >= 0);
						int spl = splhigh();
						struct tlbshootdown notif;
						notif.tlb_idx = pte->tlb_idx;
						notif.paddr = pte->paddr;
						notif.zero_mem = true;
						notif.addrspace_id = pte->as->id;
						struct cpu *cpu = thread_get_cpu(pte->cpu_idx);
						KASSERT(cpu);
						ipi_tlbshootdown(cpu, &notif);
						splx(spl);
          }
        }
      }
			lock_pagetable();
      coremap[i].state = PAGESTATE_FREE;
      coremap[i].entry = NULL;
			coremap[i].is_pinned = false;
			unlock_pagetable();
    }
  }
}

void kswapd_start(void *_data1, unsigned long _data2) {
	(void)_data1;
	(void)_data2;
	while (1) {
		thread_sleep_n_seconds(3);

    lock_pagetable();
    kswapd_age_pages();
    unlock_pagetable();

		kswapd_swapout_pages();
	}
}

paddr_t getppages(unsigned long npages, enum page_t pagetype, unsigned long *coremap_idx, bool dolock) {
	paddr_t addr;
  unsigned long page_start = 0;
  unsigned long block_count = npages;
  unsigned long i;
  if (dolock) {
		if (spinlock_do_i_hold(&stealmem_lock)) {
			dolock = false;
		} else {
			lock_pagetable();
		}
	}
	if (beforeVM){
		addr = ram_stealmem(npages);
	} else {
		// find first free n contiguous pages
		for (i=0; i<pages_in_coremap; i++) {
			if (coremap[i].state == PAGESTATE_FREE) {
				block_count--;
				if (block_count == 0){
					break;
				}
			} else {
				block_count = npages;
				page_start = i+1;
			}
		}

		if (i == pages_in_coremap) { // not found
      if (dolock) {
				unlock_pagetable();
			}
			return 0;
		} else {
			for (i=0; i<npages; i++) {
				coremap[i+page_start].state = PAGESTATE_INUSE;
				coremap[i+page_start].contained = true; // note: always true if state == INUSE
				coremap[i+page_start].partofpage = page_start; // note: if allocating 1 page, pop == idx into coremap for page
        coremap[i+page_start].is_kern_page = (pagetype == PAGETYPE_KERN);
        coremap[i+page_start].is_pinned = false;
				coremap[i+page_start].va = PADDR_TO_KVADDR(coremap[i+page_start].pa);
			}
			addr = coremap[page_start].pa;
			KASSERT(addr > 0);
		}
	}
  if (dolock) {
		unlock_pagetable();
	}
  if (coremap_idx) {
    *coremap_idx = page_start;
  }
	return addr; // physical address start (bottom addr of block of pages or page)
}

// Marks the page or pages that start at given addr as free. Doesn't zero out the page
// or pages, the caller has to do this if that's what's necessary.
static void free_pages(unsigned long addr, enum page_t pagetype, bool dolock) {
  int pop = 0;
	unsigned long i;
  if (spinlock_do_i_hold(&stealmem_lock)) {
		dolock = false;
	} else {
		dolock = true;
	}

  if (dolock) {
		lock_pagetable();
	}

	for (i=0; i<pages_in_coremap; i++){
		if (pagetype == PAGETYPE_KERN && coremap[i].va == addr && coremap[i].is_kern_page) {
			pop = coremap[i].partofpage;
			break;
		} else if (pagetype == PAGETYPE_USER && coremap[i].pa == addr && !coremap[i].is_kern_page) {
      pop = coremap[i].partofpage;
      break;
    }
	}

	if (pop && i < pages_in_coremap && coremap[i].state == PAGESTATE_FREE) {
		panic("already free");
		if (dolock) {
			unlock_pagetable();
		}
		return;
	}

	if (pop == 0 && i == pages_in_coremap) {
		//panic("page not found");
		if (dolock) {
			unlock_pagetable();
		}
		return;
	}

	int pages_freed = 0;
	while (coremap[i].contained && coremap[i].partofpage == pop) {
    if (pagetype == PAGETYPE_KERN) {
      KASSERT(coremap[i].is_kern_page);
    } else if (pagetype == PAGETYPE_USER) {
      KASSERT(!coremap[i].is_kern_page);
    } else {
      panic("invalid page type in free_pages: %d", pagetype);
    }
		coremap[i].state = PAGESTATE_FREE;
		coremap[i].contained = false;
    coremap[i].is_kern_page = false;
    if (coremap[i].entry != NULL) {
      coremap[i].entry->coremap_idx = 0;
    }
    coremap[i].entry = NULL;
    coremap[i].is_pinned = false;
		coremap[i].partofpage = 0;
		KASSERT(coremap[i].pa > 0);
		coremap[i].va = PADDR_TO_KVADDR(coremap[i].pa);
		pages_freed++;
		i++;
	}
	// page could have been freed by kswapd on a different CPU, in which case we do nothing
	if (pages_freed > 0) {
		if (pagetype == PAGETYPE_KERN) {
			DEBUG(DB_VM, "kmalloc: freed %d kernel pages\n", pages_freed);
		} else {
			DEBUG(DB_VM, "Freed %d userlevel pages\n", pages_freed);
		}
	}
  if (dolock) {
		unlock_pagetable();
	}
}

// Is the given userptr out of bounds for the process given?
// FIXME: doesn't work if proc p is a cloned proc
bool vm_userptr_oob(userptr_t ptr, struct proc *p) {
	if (ptr == (userptr_t)0) {
		return true;
	}
	struct addrspace *as = p->p_addrspace;
	vaddr_t addr = (vaddr_t)ptr;
	// NOTE: relies on first page in as->pages to be the bottom-most virtual page of memory, which it
	// always is.
	return addr < as->pages->vaddr || addr > USERSTACK;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t alloc_kpages(int npages) {
	paddr_t pa;
	DEBUG(DB_VM, "Allocating %d kernel pages\n", npages);
	pa = getppages(npages, PAGETYPE_KERN, NULL, true);

	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void free_kpages(vaddr_t addr) {
  free_pages(addr, PAGETYPE_KERN, true);
}

paddr_t alloc_upages(int npages, unsigned long *coremap_idx, bool dolock) {
	paddr_t pa;
	pa = getppages(npages, PAGETYPE_USER, coremap_idx, dolock);

	if (pa==0) {
		return 0;
	}
	return pa;
}

void free_upages(paddr_t addr, bool dolock) {
  free_pages(addr, PAGETYPE_USER, dolock);
}

paddr_t find_upage_for_entry(struct page_table_entry *pte, int page_flags, bool dolock) {
  unsigned long idx = 0;
	if (dolock)
  	lock_pagetable();
  paddr_t addr = alloc_upages(1, &idx, false);
  if (addr == 0) {
    return 0;
  }
	KASSERT(idx > 0);
  coremap[idx].entry = pte;
  coremap[idx].is_pinned = (page_flags & VM_PIN_PAGE) != 0;
	if (pte->vaddr > 0) {
		coremap[idx].va = pte->vaddr;
	}
  pte->coremap_idx = idx;
	pte->paddr = addr;
	if (dolock)
  	unlock_pagetable();
  return addr;
}

void vm_unpin_page_entry(struct page_table_entry *entry) {
  DEBUGASSERT(entry->coremap_idx > 0);
  lock_pagetable();
  coremap[entry->coremap_idx].is_pinned = false;
  unlock_pagetable();
}

int vm_pin_region(struct addrspace *as, uint32_t region_start, size_t nbytes) {
	uint32_t region_end = region_start + nbytes;
	struct page_table_entry *pte = as->pages;
	int num_entries_pinned = 0;
	while (pte) {
		if ((region_start < (pte->vaddr + PAGE_SIZE)) && region_end > pte->vaddr) {
			if (pte->coremap_idx == -1) {
				pte = pte->next;
				continue;
			}
			struct page *page = &coremap[pte->coremap_idx];
			lock_pagetable();
			page->is_pinned = true;
			unlock_pagetable();
			num_entries_pinned++;
		}
		pte = pte->next;
	}
	return num_entries_pinned;
}

// void vm_unpin_region(struct addrspace *as, uint32_t region_start, size_t nbytes) {
// 	uint32_t region_end = region_start + nbytes;
// 	struct page_table_entry *pte = as->pages;
// 	int num_entries_unpinned = 0;
// 	while (pte) {
// 		if ((region_start < (pte->vaddr + PAGE_SIZE)) && region_end > pte->vaddr) {
// 			if (pte->coremap_idx == -1) {
// 				pte = pte->next;
// 				continue;
// 			}
// 			struct page *page = &coremap[pte->coremap_idx];
// 			lock_pagetable();
// 			page->is_pinned = false;
// 			unlock_pagetable();
// 			num_entries_unpinned++;
// 		}
// 		pte = pte->next;
// 	}
// 	return num_entries_unpinned;
// }

int vm_pageout_region(struct addrspace *as, uint32_t region_start, size_t nbytes) {
	uint32_t region_end = region_start + nbytes;
	struct page_table_entry *pte = as->pages;
	int num_entries_swapped = 0;
	while (pte) {
		if ((region_start < (pte->vaddr + PAGE_SIZE)) && region_end > pte->vaddr) {
			if (pte->coremap_idx == -1) {
				pte = pte->next;
				continue;
			}
			struct page *page = &coremap[pte->coremap_idx];
			if (page->is_pinned) {
				pte = pte->next;
				continue;
			}
			int swap_res = pte_swapout(pte->as, pte, false);
			if (swap_res == 0) {
				lock_pagetable();
				page->state = PAGESTATE_FREE;
				page->entry = NULL;
				page->is_pinned = false;
				unlock_pagetable();
				num_entries_swapped++;
			}
			if (pte->tlb_idx >= 0) {
				// NOTE: assumes address space given is of the currently running process
				tlb_write(TLBHI_INVALID(pte->tlb_idx), TLBLO_INVALID(), pte->tlb_idx);
				KASSERT(pte->paddr > 0);
				memset((void*)PADDR_TO_KVADDR(pte->paddr), 0, PAGE_SIZE);
			}
		}
		pte = pte->next;
	}
	return num_entries_swapped;
}

bool vm_regions_overlap(vaddr_t reg1_btm, vaddr_t reg1_top, vaddr_t reg2_btm, vaddr_t reg2_top) {
	KASSERT(reg1_btm < reg1_top);
	KASSERT(reg2_btm < reg2_top);
	if (reg1_btm > reg2_btm) {
		if (reg1_top <= reg2_top) {
			return true;
		}
		return false;
	} else if (reg2_btm > reg1_btm) {
		if (reg2_top <= reg1_top) {
			return true;
		}
		return false;
	} else {
		return true;
	}
}

bool vm_region_contains_other(vaddr_t reg1_btm, vaddr_t reg1_top, vaddr_t reg2_btm, vaddr_t reg2_top) {
	KASSERT(reg1_btm <= reg1_top);
	KASSERT(reg2_btm <= reg2_top);
	if (reg1_btm <= reg2_btm && reg1_top >= reg2_top) {
		return true;
	} else {
		return false;
	}
}

// NOTE: unused right now, need to find way to properly implement stack growth
static struct page_table_entry *alloc_stack_page(struct addrspace *as, vaddr_t faultaddr, paddr_t *paddr_out) {
	struct page_table_entry *pte = kmalloc(sizeof(*pte));
	KASSERT(pte);
	bzero(pte, sizeof(*pte));
	vaddr_t pg_sz = (vaddr_t)PAGE_SIZE;
	pte->vaddr = ROUNDDOWN(faultaddr, pg_sz);
	pte->page_entry_type = PAGE_ENTRY_TYPE_STACK;
	pte->debug_name = as->name;
	pte->permissions = PROT_READ|PROT_WRITE|PROT_EXEC;
	pte->is_dirty = true;
	pte->coremap_idx = -1;
	pte->tlb_idx = -1;
	pte->cpu_idx = curcpu->c_number;
	paddr_t paddr = find_upage_for_entry(pte, 0, true);
	if (paddr == 0) {
		kfree(pte);
		return NULL;
	}
	memset((void*)PADDR_TO_KVADDR(paddr), 0, PAGE_SIZE);
	int old_num_pages = as_num_pages(as);
	KASSERT(pte->coremap_idx >= 0);
	pte->paddr = paddr;
	struct page_table_entry *last_stack_pte = as->stack;
	while (last_stack_pte->next && last_stack_pte->next->page_entry_type == PAGE_ENTRY_TYPE_STACK) {
		last_stack_pte = last_stack_pte->next;
	}
	KASSERT(last_stack_pte);
	struct page_table_entry *old_next = last_stack_pte->next;
	last_stack_pte->next = pte;
	pte->next = old_next;
	KASSERT(as_num_pages(as) == old_num_pages+1);
	*paddr_out = paddr;
	if (as->heap_top > pte->vaddr) {
		as->heap_top = pte->vaddr;
	}
	return pte;
}

void
vm_tlbshootdown_all(void)
{
	panic("vm tried to do tlb shootdown all?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	struct addrspace *as = proc_getas();
	if (!as) { return; }
	if (as->id != ts->addrspace_id) { return; }
	int spl = splhigh();
	if (as->running_cpu_idx != (short)curcpu->c_number) {
		DEBUG(DB_VM, "Address space no longer running, skipping shootdown notif\n");
	}
	DEBUG(DB_VM, "Received TLB shootdown notif, shooting down\n");
	KASSERT(ts->tlb_idx >= 0 && ts->tlb_idx < NUM_TLB);
	tlb_write(TLBHI_INVALID(ts->tlb_idx), TLBLO_INVALID(), ts->tlb_idx);
	if (ts->zero_mem && ts->paddr > 0) {
		memset((void*)PADDR_TO_KVADDR(ts->paddr), 0, PAGE_SIZE);
	}
	splx(spl);
}

int vm_fault(int faulttype, vaddr_t faultaddress) {
	vaddr_t stackbase, stacktop;
	paddr_t paddr = 0;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	vaddr_t initial_addr = faultaddress;
	(void)initial_addr; // for debug purposes
	faultaddress &= PAGE_FRAME;

	//DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("vm: got VM_FAULT_READONLY\n");
	case VM_FAULT_READ:
	case VM_FAULT_WRITE:
		break;
	default:
		return EINVAL;
	}

  if (curproc == kproc) {
    return EFAULT;
  }
  if (curproc == kswapproc) {
  }

	as = proc_getas();
	if (as == NULL) {
		panic("kernel fault");
		/*
		 * No address space set up. This is probably a kernel
		 * fault early in boot. Return EFAULT so as to panic
		 * instead of getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->heap_end != 0);
	KASSERT(as->heap_start != 0);
	KASSERT(as->pages != NULL);
	KASSERT(as->stack != NULL);
	KASSERT(as->heap != NULL);
	KASSERT(as->regions != NULL);
	// KASSERT((as->heap_start & PAGE_FRAME) == as->heap_start);
	// KASSERT((as->heap_end & PAGE_FRAME) == as->heap_end);
	KASSERT((as->pages->vaddr & PAGE_FRAME) == as->pages->vaddr);

	if (curproc->p_stacksize < (VM_STACKPAGES * PAGE_SIZE)) {
		curproc->p_stacksize = VM_STACKPAGES * PAGE_SIZE;
	}
	stackbase = curproc->p_stacktop - curproc->p_stacksize;
	KASSERT(stackbase > 0);
	stacktop = curproc->p_stacktop;

	struct page_table_entry *pte;

	if (faultaddress >= stackbase && faultaddress < stacktop) {
		pte = as->stack;
		while (pte != NULL) {
			if (faultaddress >= pte->vaddr && faultaddress < (pte->vaddr + PAGE_SIZE)) {
				paddr = (faultaddress - pte->vaddr) + pte->paddr;
				break;
			}
			pte = pte->next;
		}
	} else {
		pte = as->pages;
		while (pte != NULL) {
			if (faultaddress >= pte->vaddr && faultaddress < (pte->vaddr + PAGE_SIZE)) {
				paddr = (faultaddress - pte->vaddr) + pte->paddr;
				break;
			}
			pte = pte->next;
		}
	}

	// FIXME: not sure what to do here, we can't allocate arbitrary stack pages in
	// case the access isn't a stack access (for example, it's an invalid heap access).
	// There doesn't seem to be a way to support sparse stacks and to fault on invalid heap
	// accesses.
	if (false && paddr == 0 && faultaddress < stackbase && faultaddress > as->heap_end) {
		panic("allocating stack page");
		pte = alloc_stack_page(as, faultaddress, &paddr);
		if (!pte) {
			//panic("no more memory!");
			return ENOMEM;
		}
	}

	if (paddr == 0) {
		//panic("unhandled fault");
		return EFAULT;
	}

	lock_pagetable();
	if (pte && pte->is_swapped) {
		unlock_pagetable();
		int swap_res = pte_swapin(pte->as, pte, NULL);
		if (swap_res != 0) {
			panic("swapin failed!");
			return EFAULT;
		}
		KASSERT(!pte->is_swapped);
	} else {
		unlock_pagetable();
	}

  DEBUGASSERT(pte != NULL);
  // DEBUGASSERT(pte->vaddr == faultaddress);
  // DEBUGASSERT(pte->paddr == paddr);
  as_touch_pte(pte);

  // if (!pte_can_handle_fault_type(pte, faulttype)) {
  //   return EFAULT;
  // }

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

  for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "myvm: %s TLB write 0x%x -> 0x%x (index: %d)\n", as->name, faultaddress, paddr, i);
    pte->tlb_idx = (short)i;
    pte->cpu_idx = (short)curcpu->c_number;
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	DEBUG(DB_VM, "myvm (TLB eviction): 0x%x -> 0x%x\n", faultaddress, paddr);
	tlb_random(ehi, elo); // evict random TLB entry, we ran out
	pte->tlb_idx = -1;
	splx(spl);
	return 0;
}

bool vm_can_sleep(void) {
  if (CURCPU_EXISTS()) {
		/* must not hold spinlocks or be in interrupt */
		return curcpu->c_spinlocks == 0 && curthread->t_in_interrupt == 0;
	} else {
    return false;
  }
}
