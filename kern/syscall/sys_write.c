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
#include <copyinout.h>

#include <lib.h>

#include <console_lock.h>
#include <current.h>
#include <cpu.h>
#include <spl.h>
extern struct spinlock kprintf_spinlock;

// NOTE: non-zero return value is an error
int sys_write(int fd, userptr_t buf, size_t count, int *count_retval) {

  struct filedes *file_des = filetable_get(curproc, fd);
  if (!file_des) {
    DEBUG(DB_SYSCALL, "sys_write failed, fd %d not open in process %d\n", fd, curproc->pid);
    *count_retval = -1;
    return EBADF;
  }
  bool is_console = filedes_is_console(file_des);
  struct iovec iov;
  struct uio myuio;
  int errcode = 0;

  if (file_des->ftype == FILEDES_TYPE_PIPE) {
    struct pipe *writer = file_des->pipe;
    struct pipe *reader = writer->pair;
    if (!writer->is_writer || !reader) {
      DEBUG(DB_SYSCALL, "Write to pipe failed: not writer or reader doesn't exist!\n");
      *count_retval = -1;
      return EBADF;
    }
    // TODO: If the write is too large, we should write what we can and return the
    // length written
    if ((writer->bufpos + count) > writer->buflen) {
      DEBUG(DB_SYSCALL, "Write to pipe failed, bufsize not big enough\n");
      *count_retval = -1;
      return ENOMEM;
    }
    int copy_res = copyin(buf, writer->buf + writer->bufpos, count);
    if (copy_res != 0) {
      DEBUG(DB_SYSCALL, "Write to pipe failed, copyin failure: %d\n", copy_res);
      *count_retval = -1;
      return copy_res;
    }
    writer->bufpos += count;
    //DEBUG(DB_SYSCALL, "Pipe write buffer after write: %s\n", pipe->buf);
    if (reader->buflen <= writer->bufpos && reader->buflen > 0) { // reader can read now
      DEBUG(DB_SYSCALL, "Write to pipe successful, signalling reader\n");
      pipe_signal_can_read(reader); // wake up any blocked readers
      io_is_ready(curproc->pid, reader->fd, IO_TYPE_READ, count); // signal that a read is ready to select() and friends
    } else { // no blocked readers, or blocked readers want more in buffer to read at once
      DEBUG(DB_SYSCALL, "Write to pipe successful\n");
      io_is_ready(curproc->pid, reader->fd, IO_TYPE_READ, count);
    }
    *count_retval = count;
    return 0;
  }

  bool dolock = curthread->t_in_interrupt == false &&
    curthread->t_curspl == 0 && curcpu->c_spinlocks == 0;
  // avoid extraneous context switches on lock acquisition when printing to console
  // so we don't get interleaved output
  if (!is_console && file_des->lk) {
    lock_acquire(file_des->lk);
  }
  if (file_des->node == (void*)0xdeadbeef || file_des->refcount == 0) {
    DEBUG(DB_SYSCALL, "sys_write failed, fd destroyed from under us!\n");
    *count_retval = -1;
    return EBADF;
  }
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
  io_is_ready(curproc->pid, fd, IO_TYPE_READ, res);
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
