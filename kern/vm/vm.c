/*
 * vm.c
 *
 *  Created on: Apr 13, 2014
 *      Author: trinity
 */
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

static bool beforeVM = true;
static unsigned long pages_in_coremap;

// So that they can be used anywhere in other functions
paddr_t lastaddr, firstaddr, freeaddr;
struct page* coremap;

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

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

unsigned long coretotal(void) {
  return pages_in_coremap;
}

void vm_bootstrap(void) {
	int page_num, coremap_size;
	paddr_t coremapaddr, temp;

	ram_getsize(&firstaddr, &lastaddr);

	page_num = (lastaddr-firstaddr) / PAGE_SIZE;

	freeaddr = firstaddr + page_num * sizeof(struct page);
	freeaddr = ROUNDUP(freeaddr, PAGE_SIZE);// added for pr->nfree error

	coremap = (struct page*)PADDR_TO_KVADDR(firstaddr);
	coremapaddr = freeaddr - firstaddr;
	coremap_size = ROUNDUP(coremapaddr, PAGE_SIZE)/PAGE_SIZE;

	pages_in_coremap=page_num;

	for (int i=0;i<page_num;i++){

		if(i<coremap_size){
      coremap[i].is_kern_page = true;
			coremap[i].state = PAGESTATE_INUSE;
		} else {
			coremap[i].state = PAGESTATE_FREE;
			coremap[i].contained=false;
      coremap[i].is_kern_page = false;
		}

		temp = PAGE_SIZE * i + freeaddr;
		coremap[i].pa = temp;
		coremap[i].va = PADDR_TO_KVADDR(temp);

	}

	beforeVM = false;
}

paddr_t getppages(unsigned long npages, enum page_t pagetype) {
	paddr_t addr;
  spinlock_acquire(&stealmem_lock);
	if (beforeVM){
		addr = ram_stealmem(npages);
	} else {
		unsigned long page_start = 0;
		unsigned long block_count = npages;
		unsigned long i;

		// finding first free n pages
		for (i=0;i<pages_in_coremap;i++){
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
			spinlock_release(&stealmem_lock);
			return 0;
		} else {
			for (i=0; i<npages; i++) {
				coremap[i+page_start].state = PAGESTATE_INUSE;
				coremap[i+page_start].contained = true; // note: always true if state == INUSE
				coremap[i+page_start].partofpage = page_start; // note: if allocating 1 page, pop == idx into coremap for page
        coremap[i+page_start].is_kern_page = (pagetype == PAGETYPE_KERN);
			}
			addr = coremap[page_start].pa;
		}
	}

	spinlock_release(&stealmem_lock);
	return addr; // physical address start (bottom addr of block of pages or page)
}

static void free_pages(unsigned long addr, enum page_t pagetype) {
  int pop = 0;
	unsigned long i;

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
		i++;
	}

	spinlock_release(&stealmem_lock);
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t alloc_kpages(int npages) {
	paddr_t pa;
	pa = getppages(npages, PAGETYPE_KERN);

	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void free_kpages(vaddr_t addr) {
  free_pages(addr, PAGETYPE_KERN);
}

paddr_t alloc_upages(int npages) {
	paddr_t pa;
	pa = getppages(npages, PAGETYPE_USER);

	if (pa==0) {
		return 0;
	}
	return pa;
}

void free_upages(paddr_t addr) {
  free_pages(addr, PAGETYPE_USER);
}

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
  // int swap_res = pte_swapout(as, pte);
  // if (!swap_res) {
  //   panic("swap failed");
  // }

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
		DEBUG(DB_VM, "myvm: TLB write 0x%x -> 0x%x (index: %d)\n", faultaddress, paddr, i);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	DEBUG(DB_VM, "myvm (TLB eviction): 0x%x -> 0x%x\n", faultaddress, paddr);
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
