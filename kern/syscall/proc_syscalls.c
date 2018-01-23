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

#include <lib.h>

void sys_exit(int status) {
  // Detaches current thread from process, and turns it into zombie, then exits
  // from it to run other threads. Exorcise gets called once in a while (when starting
  // and switching threads, which destroys the parts of the thread that can't be destroyed
  // while it's running.

  thread_exit(status);
}

int sys_fork(int *retval) {
  int err = 0;
  int pid = proc_fork(curproc, curthread, &err);
  if (pid < 0) {
    *retval = -1;
    KASSERT(err != 0);
    return err; // error
  }
  if (pid == 0) {
    // child process shouldn't get here, it should be scheduled off the interrupt to return
    // to the trapframe with a return value of 0 and the same registers as the parent
    panic("shouldn't get here!");
  } else { // in parent
    *retval = pid;
  }
  return 0;
}

int sys_waitpid(pid_t child_pid, userptr_t exitstatus_buf, int options, int *retval) {
  int err = 0; // should be set below
  (void)options;
  int result = proc_waitpid_sleep(child_pid, &err); // FIXME: don't sleep kernel thread in interrupt handler!
  if (result == -1) {
    KASSERT(err > 0);
    DEBUG(DB_SYSCALL, "sys_waitpid failed with errno %d\n", err);
    *retval = -1;
    return err;
  }
  // NOTE: `result` here is the exitstatus of the child process
  struct uio myuio;
  struct iovec iov;
  uio_uinit(&iov, &myuio, exitstatus_buf, sizeof(result), 0, UIO_READ);
  uiomove(&result, sizeof(result), &myuio);
  *retval = 0;
  return 0;
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
