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
#include <current.h>
#include <kern/errno.h>
#include <uio.h>
#include <vnode.h>

#include <lib.h>

int
sys_write(int fd, userptr_t buf, size_t count, int *count_retval)
{
  //kprintf("sys_write\n");
  struct filedes *file_des = curproc->file_table[fd];
  if (!file_des || !file_is_open(file_des) || !file_is_writable(file_des)) {
    return EBADF;
  }
  struct iovec iov;
  struct uio myuio;
  int res = 0;
  uio_uinit(&iov, &myuio, buf, count, file_des->offset, UIO_WRITE);
  res = VOP_WRITE(file_des->node, &myuio);
  if (res != 0) {
    return res;
  }
  int bytes_written = count - myuio.uio_resid;
  //kprintf("uio bytes written: %d\n", bytes_written);
  if (bytes_written != (int)count) {
    panic("invalid write in sys_write: %d", bytes_written); // FIXME
  }
  file_des->offset = myuio.uio_offset;
  *count_retval = bytes_written;
  return 0;
}
