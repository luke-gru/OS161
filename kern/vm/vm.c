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
		if (coremap[i].state == PAGESTATE_FREE){
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
}

void kswapd_bootstrap() {
  struct proc *kswapd_proc = proc_create("kswapd");
  kswapproc = kswapd_proc;
  kswapd_proc->pid = KSWAPD_PID;
  kswapd_proc->p_parent = kproc;
  kswapd_proc->p_cwd = kproc->p_cwd;
  struct addrspace *as = as_create(kswapd_proc->p_name);
  KASSERT(as);
  kswapd_proc->p_addrspace = as;
  KASSERT(as_prepare_load(as) == 0); // allocate stack pages
  struct page_table_entry *pte = as->pages;
  char *pte_name = as->name;
  while (pte != NULL) {
    pte->debug_name = pte_name;
    KASSERT(pte->coremap_idx > 0);
    KASSERT(coremap[pte->coremap_idx].entry == pte);
    coremap[pte->coremap_idx].is_kern_page = true;
    coremap[pte->coremap_idx].is_pinned = true;
    pte = pte->next;
  }
  int fork_res = thread_fork("kswapd", kswapd_proc, kswapd_start, NULL, 0);
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
    if (!coremap[i].entry->as->is_active) {
      coremap[i].entry->coremap_idx = 0;
      coremap[i].entry = NULL;
      coremap[i].state = PAGESTATE_FREE;
      continue;
    }
    if (coremap[i].entry->page_age < VM_PAGE_AGE_MAX) {
      char *name = coremap[i].entry->debug_name;
      DEBUG(DB_VM, "\naging page %s: %d\n", name, (int)coremap[i].entry->page_age);
      coremap[i].entry->page_age++;
      as_debug(coremap[i].entry->as);
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
      if (pte->is_swapped) {
        continue;
      }
      DEBUG(DB_VM, "\nswapping out pages for %s\n", name);
      KASSERT(pte_swapout(pte->as, pte, false) == 0);
      // NOTE: only invalidate the TLB if the address space is currently running
      if (pte->as->running_cpu_idx > 0) {
        if (pte->tlb_idx >= 0 && pte->tlb_idx < NUM_TLB) {
          if (pte->cpu_idx == (short)curcpu->c_number) {
            DEBUG(DB_VM, "Invalidating page entry in TLB, zeroing memory\n");
            tlb_write(TLBHI_INVALID(pte->tlb_idx), TLBLO_INVALID(), pte->tlb_idx);
            memset((void*)PADDR_TO_KVADDR(pte->paddr), 0, PAGE_SIZE);
          } else {
            panic("needs TLB shootdown");
          }
        }
      }

      coremap[i].state = PAGESTATE_FREE;
      coremap[i].entry = NULL;
      break;
    }
  }
}

static void kswapd_swapin_async_pages() {
  return;
}

void kswapd_start(void *_data1, unsigned long _data2) {
	(void)_data1;
	(void)_data2;
	while (1) {
		clocksleep(3);
    lock_pagetable();
    kswapd_age_pages();

    unlock_pagetable();
    kswapd_swapout_pages();
    kswapd_swapin_async_pages();
    thread_yield();
	}
}

paddr_t getppages(unsigned long npages, enum page_t pagetype, unsigned long *coremap_idx, bool dolock) {
	paddr_t addr;
  unsigned long page_start = 0;
  unsigned long block_count = npages;
  unsigned long i;
  if (dolock)
    spinlock_acquire(&stealmem_lock);
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
      if (dolock)
        spinlock_release(&stealmem_lock);
			return 0;
		} else {
			for (i=0; i<npages; i++) {
				coremap[i+page_start].state = PAGESTATE_INUSE;
				coremap[i+page_start].contained = true; // note: always true if state == INUSE
				coremap[i+page_start].partofpage = page_start; // note: if allocating 1 page, pop == idx into coremap for page
        coremap[i+page_start].is_kern_page = (pagetype == PAGETYPE_KERN);
        coremap[i+page_start].is_pinned = false;
			}
			addr = coremap[page_start].pa;
		}
	}
  if (dolock)
	  spinlock_release(&stealmem_lock);
  if (coremap_idx) {
    *coremap_idx = page_start;
  }
	return addr; // physical address start (bottom addr of block of pages or page)
}

static void free_pages(unsigned long addr, enum page_t pagetype, bool dolock) {
  int pop = 0;
	unsigned long i;
  if (spinlock_do_i_hold(&stealmem_lock))
    dolock = false;

  if (dolock)
	 spinlock_acquire(&stealmem_lock);
	for (i=0; i<pages_in_coremap; i++){
		if (pagetype == PAGETYPE_KERN && coremap[i].va == addr) {
			pop = coremap[i].partofpage;
			break;
		} else if (pagetype == PAGETYPE_USER && coremap[i].pa == addr) {
      pop = coremap[i].partofpage;
      break;
    }
	}

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
		i++;
	}
  if (dolock)
	  spinlock_release(&stealmem_lock);
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t alloc_kpages(int npages) {
	paddr_t pa;
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

paddr_t find_upage_for_entry(struct page_table_entry *pte, int page_flags) {
  unsigned long idx = 0;
  spinlock_acquire(&stealmem_lock);
  paddr_t addr = alloc_upages(1, &idx, false);
  if (addr == 0) {
    return 0;
  }
  coremap[idx].entry = pte;
  coremap[idx].is_pinned = (page_flags & VM_PIN_PAGE) != 0;
  pte->coremap_idx = idx;
  spinlock_release(&stealmem_lock);
  return addr;
}

void vm_unpin_page_entry(struct page_table_entry *entry) {
  DEBUGASSERT(entry->coremap_idx > 0);
  spinlock_acquire(&stealmem_lock);
  coremap[entry->coremap_idx].is_pinned = false;
  spinlock_release(&stealmem_lock);
}

// static void vm_pin_region(vaddr_t region_start, vaddr_t region_end) {
//   // TODO
// }
//
// static void vm_unpin_region(vaddr_t region_start, vaddr_t region_end) {
//   // TODO
// }

void
vm_tlbshootdown_all(void)
{
	panic("vm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

int vm_fault(int faulttype, vaddr_t faultaddress) {
	vaddr_t stackbase, stacktop;
	paddr_t paddr = 0;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

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

	stackbase = USERSTACK - VM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

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

	if (paddr == 0) {
		return EFAULT;
	}

  DEBUGASSERT(pte != NULL);
  DEBUGASSERT(pte->vaddr == faultaddress);
  DEBUGASSERT(pte->paddr == paddr);
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
		//DEBUG(DB_VM, "myvm: TLB write 0x%x -> 0x%x (index: %d)\n", faultaddress, paddr, i);
    pte->tlb_idx = (short)i;
    pte->cpu_idx = (short)curcpu->c_number;
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	//DEBUG(DB_VM, "myvm (TLB eviction): 0x%x -> 0x%x\n", faultaddress, paddr);
	tlb_random(ehi, elo); // evict random TLB entry, we ran out
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
