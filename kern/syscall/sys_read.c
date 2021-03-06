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
#include <socket.h>

int sys_read(int fd, userptr_t buf, size_t count, int *retval) {
  struct filedes *file_des = filetable_get(curproc, fd);
  if (!file_des || !file_is_open(fd)) {
    DEBUG(DB_SYSCALL, "sys_read for fd %d failed, file not open in process %d\n", fd, curproc->pid);
    *retval = -1;
    return EBADF;
  }
  DEBUGASSERT(count > 0);
  if (file_des->ftype == FILEDES_TYPE_PIPE) {
    struct pipe *reader = file_des->pipe;
    struct pipe *writer = reader->pair;
    if (reader->is_writer || !writer) {
      DEBUG(DB_SYSCALL, "Read from pipe failed: is writer or pair vanished!\n");
      *retval = -1;
      return EBADF;
    }
    if (writer->is_closed && writer->bufpos == 0) {
      DEBUG(DB_SYSCALL, "Read from pipe got EOF, writer is closed!\n");
      *retval = 0 /*EOF*/;
      return 0;
    }
    count = count > writer->buflen ? writer->buflen : count;
    // non-blocking read, there's a big enough buffer in the write pipe
    if (writer->bufpos >= count) {
      count = count > writer->buflen ? writer->buflen : count;
      int errcode = 0;
      DEBUG(DB_SYSCALL, "Non-blocking read from pipe\n");
      int piperead_res = pipe_read_nonblock(reader, writer, buf, count, &errcode);
      if (piperead_res == -1) {
        DEBUG(DB_SYSCALL, "Read from pipe failed: %d\n", errcode);
        *retval = -1;
        return errcode;
      }
      *retval = count;
      io_is_ready(curproc->pid, writer->fd, IO_TYPE_WRITE, count); // writer can write again if blocked
      return 0;
    } else {
      if (file_des->flags & O_NONBLOCK) {
        *retval = -1;
        return EAGAIN; // read side of pipe set to non-blocking, so don't block!
      }
      int errcode = 0;
      DEBUG(DB_SYSCALL, "Blocking read from pipe\n");
      int piperead_res = pipe_read_block(reader, writer, buf, count, &errcode);
      if (piperead_res == -1) {
        DEBUG(DB_SYSCALL, "Read from pipe failed: %d\n", errcode);
        *retval = -1;
        return errcode;
      }
      *retval = count;
      io_is_ready(curproc->pid, writer->fd, IO_TYPE_WRITE, count); // writer can write again if blocked
      return 0;
    }
  }

  struct iovec iov;
  struct uio myuio;
  uio_uinit(&iov, &myuio, buf, count, file_des->offset, UIO_READ);

  int bytes_read;
  int errcode = 0;
  if (file_des->ftype == FILEDES_TYPE_SOCK) {
    bytes_read = socket_read(file_des, &myuio, &errcode);
  } else {
    bytes_read = file_read(file_des, &myuio, &errcode);
  }

  if (bytes_read < 0) {
    DEBUG(DB_SYSCALL, "sys_read for fd %d failed with error: %d, %s\n", fd, errcode, strerror(errcode));
    *retval = -1;
    return errcode;
  }
  if (bytes_read == 0) {
    DEBUG(DB_SYSCALL, "sys_read for fd %d read 0 bytes from file\n", fd);
  }
  *retval = bytes_read;
  io_is_ready(curproc->pid, fd, IO_TYPE_WRITE, bytes_read);
  return 0;
}
