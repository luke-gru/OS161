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
#include <current.h>
#include <kern/errno.h>
#include <uio.h>

#include <lib.h>

#include <console_lock.h>
#include <current.h>
#include <cpu.h>
#include <spl.h>
extern struct spinlock kprintf_spinlock;

// NOTE: non-zero return value is an error
int sys_write(int fd, userptr_t buf, size_t count, int *count_retval) {
  bool is_console = (fd == 1 || fd == 2);
  struct filedes *file_des = filetable_get(curproc, fd);
  if (!file_des) {
    DEBUG(DB_SYSCALL, "sys_write failed, fd %d not open in process %d\n", fd, curproc->pid);
    *count_retval = -1;
    return EBADF;
  }
  struct iovec iov;
  struct uio myuio;
  int errcode = 0;

  bool dolock = curthread->t_in_interrupt == false &&
    curthread->t_curspl == 0 && curcpu->c_spinlocks == 0;
  // avoid extraneous context switches on lock acquisition when printing to console
  // so we don't get interleaved output
  if (!is_console)
    lock_acquire(file_des->lk);
  // disable interrupts during writes to avoid race conditions writing to the same file
  int spl = splhigh();
  uio_uinit(&iov, &myuio, buf, count, file_des->offset, UIO_WRITE);
  if (is_console) {
    if (dolock) {
      DEBUG_CONSOLE_LOCK(fd);
    } else {
      spinlock_acquire(&kprintf_spinlock);
    }
  }

  int res = file_write(file_des, &myuio, &errcode); // releases filedes lock
  splx(spl);
  if (res == -1) {
    if (is_console) {
      if (dolock) {
        DEBUG_CONSOLE_UNLOCK();
      } else {
        spinlock_release(&kprintf_spinlock);
      }
    }
    *count_retval = -1;
    // NOTE: call to DEBUG must be after release of console lock, in case of
    // writes to console
    DEBUG(DB_SYSCALL, "sys_write failed with error: %d (%s)\n", errcode, strerror(errcode));
    return errcode;
  }
  *count_retval = res; // num bytes written
  if (is_console) {
    if (dolock) {
      DEBUG_CONSOLE_UNLOCK();
    } else {
      spinlock_release(&kprintf_spinlock);
    }
  }
  if (res == 0 && !is_console) {
    // NOTE: call to DEBUG must be after release of console lock, in case of
    // writes to console
    DEBUG(DB_SYSCALL, "sys_write reported writing 0 bytes to fd: %d\n", fd);
  }
  return 0;
}
