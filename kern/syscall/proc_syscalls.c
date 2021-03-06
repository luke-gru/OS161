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
#include <kern/errno.h>
#include <kern/unistd.h>
#include <wchan.h>
#include <mips/trapframe.h>
#include <signal.h>

// NOTE: _exit(2) syscall, NOT userland exit(3)
void sys_exit(int status) {
  // Detaches current thread from process, and turns it into zombie, then exits
  // from it to run other threads. Exorcise gets called once in a while (when starting
  // and switching threads), which destroys the parts of the thread that can't be destroyed
  // while it's running.
  thread_exit(status);
}

int sys_sigreturn(struct trapframe *tf, userptr_t sigcontext) {
  struct sigcontext sctx;
  bzero(&sctx, sizeof(sctx));
  int copy_res = copyin(sigcontext, &sctx, sizeof(sctx));
  DEBUGASSERT(copy_res == 0);
  *tf = sctx.sc_tf;
  struct sigaction *sigact_p = curproc->p_sigacts[sctx.sc_signo];
  KASSERT(sigact_p);
  // reset handler to default if necessary
  if (sigact_p->sa_flags & SA_RESETHAND) {
    sigact_p->sa_handler = SIG_DFL;
    sigact_p->sa_sigaction = NULL;
    sigact_p->sa_flags &= (~SA_RESETHAND);
  }
  struct sigaltstack *sigstack_p = curproc->p_sigaltstack;
  if (sigstack_p && (sigstack_p->ss_flags & SS_DISABLE) == 0) {
    sigstack_p->ss_flags &= (~SS_ONSTACK);
  }
  // mask out sigcantmask just to be safe, if stack was messed with
  // TODO: add cookie to sigcontext and assert it wasn't messed with!
  curthread->t_sigmask = (sctx.sc_oldmask & (~sigcantmask));
  if (curthread->t_is_suspended) {
    curthread->t_is_suspended = false;
  }
  return 0;
}

int sys_fork(struct trapframe *tf, int *retval) {
  int err = 0;
  // disable interrupts so the trapframe that we use for the parent (current) process
  // in the child doesn't point to invalid memory, which it would if the parent were to
  // run before the child and pop its trapframe stack
  int spl = splhigh();
  int flags = PROC_FORKFL_NORM;
  DEBUG(DB_SYSCALL, "process %d forking\n", curproc->pid);
  int pid = proc_fork(curproc, curthread, tf, flags, &err);
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

int sys_vfork(struct trapframe *tf, int *retval) {
  int err = 0;
  // disable interrupts so the trapframe that we use for the parent (current) process
  // in the child doesn't point to invalid memory, which it would if the parent were to
  // run before the child and pop its trapframe stack
  int spl = splhigh();
  int flags = 0;
  flags |= PROC_FORKFL_VFORK;
  DEBUG(DB_SYSCALL, "process %d vforking\n", curproc->pid);
  int pid = proc_fork(curproc, curthread, tf, flags, &err);
  if (pid < 0) {
    DEBUG(DB_SYSCALL, "sys_vfork failed: %d (%s)\n", err, strerror(err));
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
    DEBUG(DB_SYSCALL, "process %d vforked, new pid: %d\n", curproc->pid, pid);
    KASSERT(is_valid_user_pid(pid));
    *retval = pid;
    splx(spl);
    curthread->wakeup_at = -1;
    wchan_sleep(NULL, NULL); // sleep until child process wakes us upon _exit() or exec()
  }
  return 0;
}

int sys_waitpid(pid_t child_pid, userptr_t exitstatus_buf, int options, int *retval) {
  if (!is_valid_user_pid(child_pid)) {
    *retval = -1;
    return EINVAL;
  }
  int err = 0; // should be set below
  (void)options;
  DEBUG(DB_SYSCALL, "process %d waiting for %d\n", (int)curproc->pid, (int)child_pid);
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

  if (exitstatus_buf != (userptr_t)0) {
    copyout((const void*)&waitstatus, exitstatus_buf, sizeof(int));
  }

  *retval = 0;
  return 0;
}

// envp is a pointer to the bottom of the userspace **env array, which is ALWAYS 100 entries
// large, with empty entries filled with NULL. The envp array may have non-empty entries after a NULL
// entry (if that entry is not the last).
static int copy_in_environ(char **new_environ, userptr_t envp, int *num_env_vars, int *err) {
  DEBUGASSERT(new_environ);
  DEBUGASSERT(envp != (userptr_t)0);
  char *entry = *(char**)envp;
  char *buf = kmalloc(1025); // max env entry length
  if (!buf) {
    *err = ENOMEM;
    return -1;
  }
  int copy_res;
  size_t gotlen = 0;
  int num_vars_found = 0;
  for (int i = 0; i < 100; i++) {
    bzero(buf, 1025);
    gotlen = 0;
    copy_res = copyinstr((const_userptr_t)entry, buf, 1025, &gotlen);
    if (copy_res != 0) {
      *err = copy_res;
      kfree(buf);
      return -1;
    }
    if (gotlen == 0) {
      new_environ[i] = NULL;
    } else {
      new_environ[i] = kstrdup(buf);
      if (!new_environ[i]) {
        *err = ENOMEM;
        kfree(buf);
        return -1;
      }
      num_vars_found++;
    }
    entry = *((char**)envp+i+1);
    if (entry == NULL) {
      break;
    }
  }
  *num_env_vars = num_vars_found;
  kfree(buf);
  return 0;
}

int sys_execve(userptr_t filename_ubuf, userptr_t argv, userptr_t envp, int *retval) {
  int spl = splhigh();
  char fname[PATH_MAX];
  memset(fname, 0, PATH_MAX);
  size_t gotlen;
  int copy_res = copyinstr(filename_ubuf, fname, sizeof(fname), &gotlen);
  if (copy_res != 0 || gotlen <= 1) {
    DEBUG(DB_SYSCALL, "sys_execv failed to copy filename, %d (%s)\n", copy_res, strerror(copy_res));
    *retval = -1;
    splx(spl);
    if (copy_res == 0) { // empty filename_buf
      return EINVAL;
    } else {
      return copy_res;
    }
  }
  if (vm_userptr_oob(argv, curproc)) {
    panic("got here");
    *retval = -1;
    splx(spl);
    return EFAULT;
  }

  struct argvdata *argdata = argvdata_create();
  int fill_errcode = 0;
  int fill_res = argvdata_fill_from_uspace(argdata, fname, argv, &fill_errcode);
  if (fill_res == -1) {
    DEBUG(DB_SYSCALL, "sys_execv failed to copy in ARGV array, %d (%s)\n", fill_errcode, strerror(fill_errcode));
    *retval = -1;
    argvdata_destroy(argdata);
    splx(spl);
    return fill_errcode;
  }
  argvdata_debug(argdata, "exec", fname);
  if (envp == (userptr_t)0) {
    envp = curproc->p_uenviron;
  }
  if (vm_userptr_oob(envp, curproc)) {
    *retval = -1;
    argvdata_destroy(argdata);
    splx(spl);
    return EFAULT;
  }
  char **new_environ = kmalloc(101 * sizeof(char*));
  KASSERT(new_environ);
  bzero(new_environ, 101 * sizeof(char*));
  int copy_env_err = 0;
  int num_env_vars = 0;
  int copy_environ_res = copy_in_environ(new_environ, envp, &num_env_vars, &copy_env_err);
  if (copy_environ_res == -1) {
    DEBUG(DB_SYSCALL, "sys_execv failed to copy in ENV array, %d (%s)\n", copy_env_err, strerror(copy_env_err));
    *retval = -1;
    proc_free_environ(new_environ, 101);
    argvdata_destroy(argdata);
    splx(spl);
    return copy_env_err;
  }

  struct addrspace *as_old = proc_setas(NULL); // to reset in case of error
  //DEBUG(DB_SYSCALL, "sys_execv running for process %d\n", curproc->pid);
  int res = runprogram_uspace(fname, argdata, new_environ, num_env_vars);
  if (res == 0) {
    panic("shouldn't get here on success");
  }
  // failure
  DEBUG(DB_SYSCALL, "sys_execv failed to exec %s: %d (%s)\n", fname, res, strerror(res));
  proc_setas(as_old);
  as_activate();
  argvdata_destroy(argdata);
  proc_free_environ(new_environ, 101);
  splx(spl);
  *retval = -1;
  return res;
}

int sys_clone(userptr_t func, uint32_t child_stack_top, size_t stacksize, int flags, int *retval) {
  int clone_err = 0;
  struct proc *clone = proc_clone(curproc, (vaddr_t)child_stack_top, stacksize, flags, &clone_err);
  if (!clone) {
    DEBUG(DB_SYSCALL, "proc_clone failed: %d (%s)\n", clone_err, strerror(clone_err));
    *retval = -1;
    return clone_err;
  }
  int fork_err = 0;
  int clone_res = thread_fork_for_clone(
    curthread,
    clone,
    func,
    NULL,
    &fork_err
  );
  if (clone_res < 0) {
      DEBUG(DB_SYSCALL, "sys_clone: thread_fork_for_clone failed: %d (%s)\n", fork_err, strerror(fork_err));
    *retval = -1;
    return fork_err;
  }
  *retval = clone->pid;
  return 0;
}

// NOTE: sbrk here is only ever >= 0, because memory from userland free() is never
// actually given back to the OS, just written over for now and re-used in later
// allocations in the same process.
int sys_sbrk(uint32_t increment, int *retval) {
  struct addrspace *as = proc_getas();
  vaddr_t old_heapbrk = as->heap_brk;
  DEBUGASSERT(old_heapbrk > 0);
  if (increment == 0) {
    //DEBUG(DB_VM, "Giving back initial heap brk\n");
    *retval = (int)old_heapbrk;
    return 0;
  } else {
    int grow_res = as_growheap(as, increment);
    //DEBUG(DB_VM, "Growing heap by %u\n", increment);
    if (grow_res != 0) { // ENOMEM
      DEBUG(DB_VM, "Growing heap failed with: %d\n", grow_res);
      *retval = -1;
      return grow_res;
    }
    //DEBUG(DB_VM, "Old heapbrk from sbrk: %d\n", (int)old_heapbrk);
    *retval = (int)old_heapbrk;
    return 0;
  }
}

int sys_mmap(size_t nbytes, int prot, int flags, int fd, off_t offset, int *retval) {
  int errcode = 0;
  vaddr_t mmap_start = 0;
  int mmap_res = as_add_mmap(curproc->p_addrspace, nbytes, prot, flags, fd, offset, &mmap_start, &errcode);
  if (mmap_res == 0) { // success
    *retval = (int)mmap_start;
    return 0;
  } else {
    *retval = -1; // MAP_FAILED value in userland
    return errcode;
  }
}

int sys_msync(uint32_t startaddr, size_t length, int flags, int *retval) {
  int errcode = 0;
  struct addrspace *as = curproc->p_addrspace;
  if (startaddr % PAGE_SIZE != 0 || length == 0) {
    *retval = -1;
    return EINVAL;
  }
  struct mmap_reg *mmap = as_mmap_for_region(as, startaddr, startaddr+length);
  if (!mmap) {
    *retval = -1;
    return ENOMEM;
  }
  unsigned start_page = 0;
  struct page_table_entry *pte = mmap->ptes;
  for (unsigned i = 0; i < mmap->num_pages; i++) {
    if (startaddr > pte->vaddr) {
      pte = pte->next;
      KASSERT(pte);
      start_page++;
    } else {
      DEBUGASSERT(startaddr == pte->vaddr);
    }
  }
  int mmap_sync_res = as_sync_mmap(as, mmap, start_page, length, flags, &errcode);
  if (mmap_sync_res != 0) {
    *retval = -1;
    return errcode;
  }
  *retval = 0;
  return 0;
}

int sys_munmap(uint32_t startaddr, int *retval) {
  int errcode = 0;
  if (startaddr % PAGE_SIZE != 0) {
    *retval = -1;
    return EINVAL;
  }
  struct addrspace *as = curproc->p_addrspace;
  struct mmap_reg *reg = as->mmaps;
  bool created_map = false;
  int res = 0;
  while (reg && reg->start_addr != startaddr) {
    reg = reg->next;
  }
  if (!reg) {
    *retval = -1;
    return EINVAL;
  }
  created_map = reg->opened_by == curproc->pid;
  if (reg->backing_obj && ((reg->flags & MAP_SHARED) != 0)) {
    DEBUG(DB_SYSCALL, "Syncing mmap before unmapping it because it's shared\n");
    int sync_res = as_sync_mmap(as, reg, 0, reg->valid_end_addr - reg->start_addr, 0, &errcode);
    if (sync_res != 0) {
      DEBUG(DB_SYSCALL, "Syncing mmap failed with %d (%s)\n", errcode, strerror(errcode));
    }
  }
  if (created_map) {
    DEBUG(DB_SYSCALL, "Removing mmap from process %d (as_rm_mmap)\n", (int)curproc->pid);
    lock_pagetable();
    res = as_rm_mmap(as, reg);
    unlock_pagetable();
    if (res == 0) {
      as_free_mmap(as, reg);
    } else {
      DEBUG(DB_SYSCALL, "Error trying to free mmapped physical pages: %d (%s)\n", res, strerror(res));
    }
  } else {
    DEBUG(DB_SYSCALL, "Removing mmap from process %d (as_free_mmap)\n", (int)curproc->pid);
    res = as_free_mmap(as, reg);
    if (res != 0) {
      DEBUG(DB_SYSCALL, "Error trying to remove mmap page table entries: %d (%s)\n", res, strerror(res));
    }
  }
  if (res == 0) {
    *retval = 0;
    return 0;
  } else {
    errcode = res;
    *retval = -1;
    return errcode;
  }
}

// set user-supplied signal handler for given signal
// NOTE: user_handler can be a ptr to a function or the special values
// SIG_DFL or SIG_IGN
int sys_signal(int signo, vaddr_t user_handler, int *retval) {
  if (signo < 1 || signo > NSIG || signo == SIGKILL || signo == SIGSTOP) {
    *retval = (int)SIG_ERR;
    return EINVAL;
  }
  __sigfunc prev_handler = curproc->p_sigacts[signo]->sa_handler;
  if (!prev_handler) {
    prev_handler = SIG_DFL;
  }
  curproc->p_sigacts[signo]->sa_handler = (__sigfunc)user_handler;
  *retval = (int)prev_handler;
  return 0;
}

// Set and/or get user-supplied signal handler.
int sys_sigaction(int signo, const_userptr_t action, userptr_t old_action, int *retval) {
  if (signo < 1 || signo > NSIG || signo == SIGKILL || signo == SIGSTOP) {
    *retval = -1;
    return EINVAL;
  }
  struct sigaction *cursigact_p = curproc->p_sigacts[signo];
  struct sigaction cursigact;
  if (action == 0 && old_action == 0) {
    *retval = -1;
    return EINVAL;
  }
  if (!cursigact_p) {
    bzero(&cursigact, sizeof(cursigact));
  } else {
    cursigact = *cursigact_p; // copy fields
  }
  int copy_res;
  if (old_action) {
    copy_res = copyout(&cursigact, old_action, sizeof(cursigact));
    if (copy_res != 0) {
      *retval = -1;
      return EFAULT;
    }
    if (!action) {
      *retval = 0;
      return 0;
    }
  }
  struct sigaction *newsigact_p = kmalloc(sizeof(*newsigact_p));
  KASSERT(newsigact_p);
  copy_res = copyin(action, newsigact_p, sizeof(*newsigact_p));
  if (copy_res != 0) {
    kfree(newsigact_p);
    *retval = -1;
    return EFAULT;
  }
  if ((newsigact_p->sa_flags & SA_SIGINFO) && !newsigact_p->sa_sigaction) {
    kfree(newsigact_p);
    *retval = -1;
    return EINVAL;
  }

  curproc->p_sigacts[signo] = newsigact_p;

  return 0;
}

int sys_sigaltstack(const_userptr_t u_newstack, userptr_t u_oldstack, int *retval) {
  struct sigaltstack *curstack_p = curproc->p_sigaltstack;
  struct sigaltstack newaltstack;
  bzero(&newaltstack, sizeof(newaltstack));
  int copy_res;
  if (u_oldstack != (userptr_t)0) {
    if (!curstack_p) {
      copy_res = copyout(&newaltstack, u_oldstack, sizeof(newaltstack)); // copy out a zeroed out struct
    } else {
      copy_res = copyout(curstack_p, u_oldstack, sizeof(*curstack_p));
    }
    if (copy_res != 0) {
      DEBUG(DB_SYSCALL, "sigaltstack: copyout failed\n");
      *retval = -1;
      return EFAULT;
    }
    if (u_newstack == (const_userptr_t)0) {
      *retval = 0;
      return 0;
    }
  }

  if (u_newstack == (const_userptr_t)0) {
    *retval = -1;
    return EINVAL;
  }

  // Can't update the alternate stack if we're executing on it...
  if (curstack_p && (curstack_p->ss_flags & SS_ONSTACK)) {
    *retval = -1;
    return EPERM;
  }

  copy_res = copyin(u_newstack, &newaltstack, sizeof(newaltstack));
  if (copy_res != 0) {
    DEBUG(DB_SYSCALL, "sigaltstack: copyin failed\n");
    *retval = -1;
    return EFAULT;
  }

  if (newaltstack.ss_sp == NULL) {
    DEBUG(DB_SYSCALL, "sigaltstack: ss_sp = NULL ptr\n");
    *retval = -1;
    return EINVAL;
  }
  vaddr_t stackbtm = (vaddr_t)newaltstack.ss_sp;
  vaddr_t stacktop = stackbtm + newaltstack.ss_size;
  struct addrspace *as = proc_getas();
  if (stackbtm < as->heap_start || stacktop > as->heap_top) {
    DEBUG(DB_SYSCALL, "sigaltstack: stack out of bounds\n");
    *retval = -1;
    return EFAULT;
  }
  if (newaltstack.ss_size < MINSIGSTKSZ) {
    DEBUG(DB_SYSCALL, "sigaltstack: stack size too low\n");
    *retval = -1;
    return ENOMEM;
  }
  struct sigaltstack *newaltstack_p = kmalloc(sizeof(newaltstack));
  KASSERT(newaltstack_p);
  memcpy(newaltstack_p, &newaltstack, sizeof(newaltstack));
  curproc->p_sigaltstack = newaltstack_p;
  *retval = 0;
  return 0;
}

// Causes current thread to sleep until it catches a signal and runs its handler
int sys_pause(int *retval) {
  curthread->t_is_paused = true;
  thread_stop();
  *retval = -1;
  return EINTR;
}

int sys_sigsuspend(const_userptr_t u_newsigmask, int *retval) {
  if (u_newsigmask == (const_userptr_t)0) {
    *retval = -1;
    return EFAULT;
  }
  sigset_t newsigmask;
  int copy_res;
  sigemptyset(&newsigmask);
  copy_res = copyin(u_newsigmask, &newsigmask, sizeof(sigset_t));
  if (copy_res != 0) {
    *retval = -1;
    return EFAULT;
  }
  newsigmask &= (~sigcantmask);
  curthread->t_is_suspended = true;
  curthread->t_sigoldmask = curthread->t_sigmask;
  curthread->t_sigmask = newsigmask;
  if (thread_has_pending_unblocked_signal()) {
    // don't stop the thread, run the signal handlers instead
    // FIXME: only do this if the pending signals aren't set to ignore
  } else {
    thread_stop();
  }
  // NOTE: the thread is marked as unsuspended only after the handler runs (in sys_sigreturn)
  DEBUGASSERT(curthread->t_is_suspended);
  *retval = -1;
  return EINTR;
}

// TODO: sys_kill

int sys_sigpending(userptr_t u_sigset, int *retval) {
  if (u_sigset == (userptr_t)0) {
    *retval = -1;
    return EFAULT;
  }
  sigset_t sigspending;
  sigemptyset(&sigspending);
  struct thread *t = curthread;
  int copy_res;
  for (int signo = 1; signo <= NSIG; signo++) {
    if (t->t_pending_signals[signo]) {
      sigaddset(&sigspending, signo);
    }
  }

  copy_res = copyout(&sigspending, u_sigset, sizeof(sigset_t));
  if (copy_res != 0) {
    *retval = -1;
    return EFAULT;
  }
  *retval = 0;
  return 0;
}

int sys_sigprocmask(int how, const_userptr_t u_set, userptr_t u_oldset, int *retval) {
  int copy_res;
  if (u_oldset != (userptr_t)0) {
    copy_res = copyout(&curthread->t_sigmask, u_oldset, sizeof(sigset_t));
    if (copy_res != 0) {
      *retval = -1;
      return EFAULT;
    }
    if (u_set == (const_userptr_t)0) {
      *retval = 0;
      return 0;
    }
  }
  if (!u_set) { // do nothing
    *retval = 0;
    return 0;
  }
  sigset_t set;
  copy_res = copyin(u_set, &set, sizeof(sigset_t));
  if (copy_res != 0) {
    *retval = -1;
    return EFAULT;
  }
  set &= (~sigcantmask);
  switch(how) {
    case SIG_BLOCK:
      curthread->t_sigmask |= set;
      break;
    case SIG_UNBLOCK:
      curthread->t_sigmask &= (~set);
      break;
    case SIG_SETMASK:
      curthread->t_sigmask = set;
      break;
    default:
    *retval = -1;
    return EINVAL;
  }
  *retval = 0;
  return 0;
}

int sys_getpid(int *retval) {
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
  KASSERT(curthread->t_iplhigh_count == 0);
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
