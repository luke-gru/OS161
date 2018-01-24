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

#include <lib.h>

void sys_exit(int status) {
  // Detaches current thread from process, and turns it into zombie, then exits
  // from it to run other threads. Exorcise gets called once in a while (when starting
  // and switching threads, which destroys the parts of the thread that can't be destroyed
  // while it's running.

  thread_exit(status);
}

int sys_fork(struct trapframe *tf, int *retval) {
  int err = 0;
  // disable interrupts so the trapframe that we use for the parent (current) process
  // in the child doesn't point to invalid memory, which it would if the parent were to
  // run before the child and pop its trapframe stack
  int spl = splhigh();
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
  int result = proc_waitpid_sleep(child_pid, &err);
  if (result == -1) {
    KASSERT(err > 0);
    DEBUG(DB_SYSCALL, "sys_waitpid failed with errno %d (%s)\n", err, strerror(err));
    *retval = -1;
    return err;
  }
  int waitstatus = _MKWAIT_EXIT(result);
  copyout((const void*)&waitstatus, exitstatus_buf, sizeof(int));
  *retval = 0;
  return 0;
}

int sys_execv(userptr_t filename_ubuf, userptr_t argv, userptr_t env, int *retval) {
  (void)env; // TODO
  char fname[PATH_MAX+1];
  memset(fname, 0, PATH_MAX+1);
  int copy_res = copyinstr(filename_ubuf, fname, PATH_MAX+1, NULL);
  if (copy_res != 0) {
    DEBUG(DB_SYSCALL, "sys_execv failed to copy filename, %d (%s)\n", copy_res, strerror(copy_res));
    *retval = -1;
    return copy_res;
  }
  int spl = splhigh(); // disable interrupts
  struct addrspace *as_old = proc_setas(NULL); // to reset in case of error
  DEBUG(DB_SYSCALL, "sys_execv running for process %d\n", curproc->pid);
  int res = runprogram_uspace(fname, argv, spl);
  if (res == 0) {
    panic("shouldn't get here on success");
  }
  DEBUG(DB_SYSCALL, "sys_execv failed to exec: %d (%s)\n", res, strerror(res));
  proc_setas(as_old);
  as_activate();
  splx(spl);
  *retval = -1;
  return res;
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
