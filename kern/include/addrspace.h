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

#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

/*
 * Address space structure and operations.
 */


#include <vm.h>
#include "opt-dumbvm.h"
#include <synch.h>

struct vnode;
struct page;

unsigned long last_addrspace_id;

enum page_entry_type {
  PAGE_ENTRY_TYPE_EXEC = 1, // page for memory loaded from executable (data and code)
  PAGE_ENTRY_TYPE_STACK = 2, // page for user stack
  PAGE_ENTRY_TYPE_HEAP = 3, // page for user heap
  PAGE_ENTRY_TYPE_MMAP = 4 // page for mmapped regions
};
#define PAGE_ENTRY_FLAG_INJECTED 2

struct page_table_entry {
  struct addrspace *as;
  char *debug_name; // same name as addrspace and process
	vaddr_t vaddr;
	paddr_t paddr;
	int permissions; // read/write/execute memory permissions
  int flags;
  enum page_entry_type page_entry_type;
  bool is_swapped; // is this entry swapped to disk and not resident in physical memory
  // Dirty means the contents of the page are different from the saved contents on disk
  // TODO: in vm_fault, set all TLB entries to read-only initially, then a write gets a
  // fault and we mark it as dirty and continue.
  bool is_dirty;
  //int32_t last_fault_access; // timestamp
  off_t swap_offset; // byte offset into swap file, if num_swaps > 0
  unsigned short num_swaps; // number of times this page entry was swapped out
  // index into coremap. 0 means the page isn't resident in physical memory, We can
  // treat 0 as an invalid index because the first few pages in the coremap are for the
  // coremap itself, and they contain no page table entries
  long coremap_idx;
  unsigned short page_age; // 0-VM_PAGE_AGE_MAX, age is incremented every kswapd loop
  short tlb_idx; // TLB index if it's resident in memory and hit the TLB, -1 if not. Invalidated every
  // activation of the thread (quantum)
  short cpu_idx; // CPU index for TLB entry (0-3)
  //short swap_errors; // number of errors trying to swap

	struct page_table_entry *next;
};

// linked list of pids
struct pidlist {
  pid_t pid;
  struct pidlist *next;
};

// linked list of memory-mapped regions
struct mmap_reg {
  int prot_flags; // protection flags (R/W/X)
  int flags; // visibility flags (shared/private) and whether it's anonymous or not (not backed by file)
  struct page_table_entry *ptes; // page table entries for mapping
  unsigned num_pages;
  int fd; // if this is a file-backed mmap call
  vaddr_t start_addr;
  vaddr_t end_addr;
  vaddr_t valid_end_addr;
  pid_t opened_by;
  struct pidlist *pids_sharing; // only non-null if opened_by == curproc->pid and MAP_SHARED is given in flags
  struct mmap_reg *next;
};

struct regionlist {
  vaddr_t vbase;
  size_t npages;
  int permissions;
  struct regionlist *next;
  struct regionlist *last;
};

/*
 * Address space - data structure associated with the virtual memory
 * space of a process.
 */

struct addrspace {
#if OPT_DUMBVM
        char *name;
        vaddr_t as_vbase1;
        paddr_t as_pbase1;
        size_t as_npages1;
        vaddr_t as_vbase2;
        paddr_t as_pbase2;
        size_t as_npages2;
        paddr_t as_stackpbase;
#else
        char *name; // used for debugging purposes
        unsigned long id; // address space ID
        pid_t pid;        // process ID of address space (same as process)
        struct page_table_entry *pages; // all pages in address space (including stack, heap and data and executable)
        struct page_table_entry *heap; // beginning of heap (bottom address, heap grows up)
        struct page_table_entry *stack; // top of stack (bottom address, stack grows down)
        struct mmap_reg *mmaps; // memory mapped regions (see mmap syscall)
        struct regionlist *regions;
        vaddr_t heap_start; // start of heap, stays the same, never increases after executable load
        vaddr_t heap_end; // end of heap pages, always divisible by PAGE_SIZE
        vaddr_t heap_brk; // end of heap available to userspace, growable with sbrk()
        vaddr_t heap_top; // top of heap, decreases if stack size increases or mmapped regions are added
        vaddr_t sigretcode; // sigreturncode
        struct spinlock spinlock;
        time_t last_activation; // last time that this address space began its time slice
        bool destroying;
        bool is_active; // Doesn't mean it's currently running, just that it ran at least one time slice and hasn't exited
        bool no_heap_alloc; // For now, used for processes internal to the kernel that run in their own special "userspace"
        short running_cpu_idx; // If the address space's process is currently running, this is the cpu idx (0-3) of the CPU
        short refcount; // address spaces are shared with the clone() syscall (used for userspace threads, which share a virtual address space)

#endif
};

/*
 * Functions in addrspace.c:
 *
 *    as_create - create a new empty address space. You need to make
 *                sure this gets called in all the right places. You
 *                may find you want to change the argument list. May
 *                return NULL on out-of-memory error.
 *
 *    as_copy   - create a new address space that is an exact copy of
 *                an old one. Probably calls as_create to get a new
 *                empty address space and fill it in, but that's up to
 *                you.
 *
 *    as_activate - make curproc's address space the one currently
 *                "seen" by the processor.
 *
 *    as_deactivate - unload curproc's address space so it isn't
 *                currently "seen" by the processor. This is used to
 *                avoid potentially "seeing" it while it's being
 *                destroyed.
 *
 *    as_destroy - dispose of an address space. You may need to change
 *                the way this works if implementing user-level threads.
 *
 *    as_define_region - set up a region of memory within the address
 *                space.
 *
 *    as_prepare_load - this is called before actually loading from an
 *                executable into the address space.
 *
 *    as_complete_load - this is called when loading from an executable
 *                is complete.
 *
 * Note that when using dumbvm, addrspace.c is not used and these
 * functions are found in dumbvm.c.
 */

struct addrspace *as_create(char *name);
int               as_copy(struct addrspace *src, struct addrspace **ret);
void              as_activate(void);
void              as_deactivate(void);
void              as_destroy(struct addrspace *);

int               as_define_region(struct addrspace *as,
                                   vaddr_t vaddr, size_t sz,
                                   int readable,
                                   int writeable,
                                   int executable);
int               as_prepare_load(struct addrspace *as);
int               as_complete_load(struct addrspace *as);

vaddr_t           as_heapend(struct addrspace *as);
int               as_growheap(struct addrspace *as, size_t bytes);
bool              as_heap_region_exists(struct addrspace *as, vaddr_t btm, vaddr_t top);

bool              pte_can_handle_fault_type(struct page_table_entry *pte, int faulttype);
void              as_touch_pte(struct page_table_entry *pte);
int               as_swapout(struct addrspace *as);
int               as_swapin(struct addrspace *as);
int               pte_swapout(struct addrspace *as, struct page_table_entry *pte, bool zero_fill_mem);
int               pte_swapin(struct addrspace *as, struct page_table_entry *pte, struct page **into_page);
void              as_debug(struct addrspace *as);
void              as_lock_all(void);
void              as_unlock_all(void);
bool              as_is_destroyed(struct addrspace *as);
int               as_num_pages(struct addrspace *as);

int as_add_mmap(
  struct addrspace *as, size_t len, int prot,
  int flags, int fd, off_t file_offset, vaddr_t *mmap_startaddr, int *errcode
);
int as_rm_mmap(struct addrspace *as, struct mmap_reg *reg);
int as_free_mmap(struct addrspace *as, struct mmap_reg *reg);
void mmap_add_shared_pid(struct mmap_reg *mmap, pid_t pid);

/*
 * Functions in loadelf.c
 *    load_elf - load an ELF user program executable into the current
 *               address space. Returns the entry point (initial PC)
 *               in the space pointed to by ENTRYPOINT.
 */

int load_elf(struct vnode *v, vaddr_t *entrypoint);


#endif /* _ADDRSPACE_H_ */
