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
#include <syscall.h>
#include <proc.h>
#include <vfs.h>
#include <thread.h>
#include <current.h>
#include <uio.h>
#include <copyinout.h>
#include <kern/wait.h>
#include <spl.h>
#include <addrspace.h>
#include <argvdata.h>
#include <clock.h>

#include <lib.h>

void sys_exit(int status) {
  // Detaches current thread from process, and turns it into zombie, then exits
  // from it to run other threads. Exorcise gets called once in a while (when starting
  // and switching threads), which destroys the parts of the thread that can't be destroyed
  // while it's running.

  thread_exit(status);
}

int sys_fork(struct trapframe *tf, int *retval) {
  int err = 0;
  // disable interrupts so the trapframe that we use for the parent (current) process
  // in the child doesn't point to invalid memory, which it would if the parent were to
  // run before the child and pop its trapframe stack
  int spl = splhigh();

  DEBUG(DB_SYSCALL, "process %d forking\n", curproc->pid);
  int pid = proc_fork(curproc, curthread, tf, &err);
  if (pid < 0) {
    DEBUG(DB_SYSCALL, "sys_fork failed: %d (%s)\n", err, strerror(err));
    *retval = -1;
    KASSERT(err != 0);
    splx(spl);
    return err; // error
  }
  if (pid == 0) {
    // child process shouldn't get here, it should be scheduled off the interrupt to return
    // to the syscall trapframe with a return value of 0 and the same CPU registers as the parent
    panic("shouldn't get here!");
  } else { // in parent
    DEBUG(DB_SYSCALL, "process %d forked, new pid: %d\n", curproc->pid, pid);
    KASSERT(is_valid_user_pid(pid));
    *retval = pid;
  }
  splx(spl);
  thread_yield(); // make sure child runs before us (FIXME)
  return 0;
}

int sys_waitpid(pid_t child_pid, userptr_t exitstatus_buf, int options, int *retval) {
  int err = 0; // should be set below
  (void)options;
  DEBUG(DB_SYSCALL, "process %d waiting for %d\n", (int)curproc->pid, (int)child_pid);
  // vm_pin_region(exitstatus_buf, exitstatus_buf + sizeof(int));
  int result = proc_waitpid_sleep(child_pid, &err);
  if (result == -1) {
    KASSERT(err > 0);
    DEBUG(DB_SYSCALL, "sys_waitpid failed with errno %d (%s)\n", err, strerror(err));
    *retval = -1;
    return err;
  }
  int waitstatus = _MKWAIT_EXIT(result);
  DEBUG(DB_SYSCALL, "process %d finished waiting for %d, status: %d\n",
    (int)curproc->pid,
    (int)child_pid,
    waitstatus
  );

  copyout((const void*)&waitstatus, exitstatus_buf, sizeof(int));
  // vm_unpin_region(exitstatus_buf, exitstatus_buf + sizeof(int));
  *retval = 0;
  return 0;
}

int sys_execv(userptr_t filename_ubuf, userptr_t argv, int *retval) {
  int spl = splhigh();
  char fname[PATH_MAX+1];
  memset(fname, 0, PATH_MAX+1);
  int copy_res = copyinstr(filename_ubuf, fname, PATH_MAX+1, NULL);
  if (copy_res != 0) {
    DEBUG(DB_SYSCALL, "sys_execv failed to copy filename, %d (%s)\n", copy_res, strerror(copy_res));
    *retval = -1;
    splx(spl);
    return copy_res;
  }
  struct argvdata *argdata = argvdata_create();
  argvdata_fill_from_uspace(argdata, fname, argv);
  argvdata_debug(argdata, "exec", fname);

  struct addrspace *as_old = proc_setas(NULL); // to reset in case of error
  //DEBUG(DB_SYSCALL, "sys_execv running for process %d\n", curproc->pid);
  int res = runprogram_uspace(fname, argdata);
  if (res == 0) {
    panic("shouldn't get here on success");
  }
  // failure
  DEBUG(DB_SYSCALL, "sys_execv failed to exec %s: %d (%s)\n", fname, res, strerror(res));
  proc_setas(as_old);
  as_activate();
  argvdata_destroy(argdata);
  splx(spl);
  *retval = -1;
  return res;
}

// NOTE: sbrk here is only ever >= 0, because memory from userland free() is never
// actually given back to the OS, just written over for now and re-used in later
// allocations in the same process.
int sys_sbrk(uint32_t increment, int *retval) {
  struct addrspace *as = proc_getas();
  vaddr_t old_heapbrk = as->heap_brk;
  DEBUGASSERT(old_heapbrk > 0);
  if (increment == 0) {
    DEBUG(DB_VM, "Giving back initial heap brk\n");
    *retval = (int)old_heapbrk;
    return 0;
  } else {
    int grow_res = as_growheap(as, increment);
    DEBUG(DB_VM, "Growing heap by %u\n", increment);
    if (grow_res != 0) { // ENOMEM
      DEBUG(DB_VM, "Growing heap failed with: %d\n", grow_res);
      *retval = -1;
      return grow_res;
    }
    DEBUG(DB_VM, "Old heapbrk from sbrk: %d\n", (int)old_heapbrk);
    *retval = (int)old_heapbrk;
    return 0;
  }
}

int sys_getpid(int *retval)
{
  *retval = (int)curproc->pid;
  return 0;
}

int sys_getcwd(userptr_t buf, size_t buflen, int *retval)
{
  struct iovec iov;
  struct uio useruio;
  int result;

  uio_uinit(&iov, &useruio, buf, buflen, 0, UIO_READ);

  result = vfs_getcwd(&useruio);
  if (result) {
    return result;
  }

  *retval = buflen - useruio.uio_resid;

  return 0;
}

int sys_sleep(int seconds, int *retval) {
  if (seconds <= 0) {
    *retval = 0;
    return 0;
  }
  thread_sleep_n_seconds(seconds);
  *retval = 0;
  return 0;
}

int sys_pageout_region(uint32_t startaddr, size_t nbytes, int *retval) {
  struct addrspace *as = proc_getas();
  KASSERT(as);
  int res = vm_pageout_region(as, startaddr, nbytes);
  if (res <= 0) {
    *retval = -1;
  } else {
    *retval = res; // number of pages swapped out
  }
  return 0;
}

int sys_lock_region(uint32_t startaddr, size_t nbytes, int *retval) {
  struct addrspace *as = proc_getas();
  KASSERT(as);
  int res = vm_pin_region(as, startaddr, nbytes);
  if (res <= 0) {
    *retval = -1;
  } else {
    *retval = res; // number of pages pinned
  }
  return 0;
}
