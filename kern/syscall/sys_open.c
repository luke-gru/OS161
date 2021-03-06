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
#include <copyinout.h>
#include <kern/errno.h>
#include <kern/seek.h>

#include <lib.h>

int sys_open(userptr_t filename, int openflags, mode_t mode, int *fd_retval) {
  char fname[FPATH_MAX];
  int copy_res = copyinstr(filename, fname, sizeof(fname), NULL);
  if (copy_res != 0) {
    DEBUG(DB_SYSCALL, "sys_open failed to copy filename, error: %d (%s)\n", copy_res, strerror(copy_res));
    *fd_retval = -1;
    return ENOMEM;
  }
  int err = 0;
  int fd = file_open(fname, openflags, mode, &err);
  if (fd == -1 || err != 0) {
    DEBUG(DB_SYSCALL, "sys_open failed with error: %d (%s)\n", err, strerror(err));
    *fd_retval = -1;
    return err;
  }
  struct filedes *desc = filetable_get(curproc, fd);
  DEBUGASSERT(desc != NULL);
  DEBUGASSERT(desc->offset == 0);
  DEBUGASSERT(desc->refcount == 1);
	*fd_retval = fd;
  return 0;
}
