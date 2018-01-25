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
#include <limits.h>
#include <vfs.h>
#include <copyinout.h>
#include <proc.h>
#include <lib.h>
#include <stat.h>
#include <kern/errno.h>
#include <kern/stat.h>
#include <current.h>
#include <vnode.h>
#include <spl.h>

int sys_chdir(userptr_t dirbuf, int *retval) {
  char fname[PATH_MAX];
  int copy_res = copyinstr(dirbuf, fname, sizeof(fname), NULL);
  if (copy_res != 0) {
    *retval = -1;
    return copy_res;
  }
  //kprintf("dir: '%s'\n", fname);
  if (strcmp(fname, "") == 0) {
    *retval = -1;
    return EINVAL;
  }
  *retval = vfs_chdir(fname); // sets curproc->p_pwd
  if (*retval != 0) {
    DEBUG(DB_SYSCALL, "Error in sys_chdir: process %d: code=%d (%s)\n",
      curproc->pid, *retval, strerror(*retval));
  }
  return *retval; // 0 on success
}

int sys_close(int fd, int *retval) {
  (void)fd;
  int res = file_close(fd);
  if (res != 0) {
    DEBUG(DB_SYSCALL, "Error in sys_close: fd %d for process %d: code=%d (%s)\n",
      fd, curproc->pid, res, strerror(res));
  }
  *retval = res;
  return res;
}

int sys_fstat(int fd, userptr_t stat_buf, int *retval) {
  struct filedes *file_des = filetable_get(curproc, fd);
  if (!file_des) {
    DEBUG(DB_SYSCALL, "Error in sys_fstat: fd %d for process %d: file not open\n",
      fd, curproc->pid);
    *retval = -1;
    return EBADF;
  }
  int errcode = 0;
  struct stat st;
  int res = filedes_stat(file_des, &st, &errcode);
  if (res != 0) {
    DEBUG(DB_SYSCALL, "Error in sys_fstat: fd %d for process %d: code=%d (%s)\n",
      fd, curproc->pid, errcode, strerror(errcode));
    *retval = -1;
    return errcode;
  }
  copyout(&st, stat_buf, sizeof(struct stat));
  *retval = 0;
  return 0;
}

int sys_lseek(int fd, off_t offset, int whence, int *retval) {
  struct filedes *file_des = filetable_get(curproc, fd);
  if (!file_des) {
    DEBUG(DB_SYSCALL, "Error in sys_lseek: fd %d for process %d, file not open\n",
      fd, curproc->pid);
    *retval = -1;
    return EBADF;
  }
  int errcode = 0;
  int res = file_seek(file_des, offset, whence, &errcode);
  if (res != 0) {
    DEBUG(DB_SYSCALL, "Error in sys_lseek: fd %d for process %d, code=%d, err=%s\n",
      fd, curproc->pid, errcode, strerror(errcode));
    *retval = -1;
    return errcode;
  }
  *retval = (int)file_des->offset;
  return 0;
}

int sys_remove(userptr_t filename, int *retval) {
  char path[PATH_MAX];
  copyinstr(filename, (void*)&path, sizeof(path), NULL);
  if (strlen(path) == 0) {
    *retval = -1;
    return EBADF;
  }
  int res = file_unlink(path);
  if (res != 0) {
    DEBUG(DB_SYSCALL, "Error in sys_remove: path: \"%s\" for process %d, code=%d, err=%s\n",
      path, curproc->pid, res, strerror(res));
      *retval = -1;
      return res;
  }
  *retval = 0;
  return 0;
}

int sys_dup(int fd, int *retval) {
  int spl = splhigh();
  struct filedes *file_des = filetable_get(curproc, fd);
  if (!file_des) {
    DEBUG(DB_SYSCALL, "Error in sys_dup: fd %d for process %d: file not open\n",
      fd, curproc->pid);
    *retval = -1;
    splx(spl);
    return EBADF;
  }
  lock_acquire(file_des->lk);
  int newfd = filetable_put(curproc, file_des, -1);
  if (newfd < 0) {
    DEBUG(DB_SYSCALL, "Error in sys_dup: fd %d for process %d: too many open files\n",
      fd, curproc->pid);
      *retval = -1;
      lock_release(file_des->lk);
      splx(spl);
      return EMFILE;
  }
  file_des->refcount++;
  *retval = newfd;
  lock_release(file_des->lk);
  splx(spl);
  return 0;
}

int sys_dup2(int oldfd, int newfd, int *retval) {
  (void)oldfd;
  (void)newfd;
  (void)retval;
  return 0;
}

int sys_mkdir(userptr_t pathname, mode_t mode, int *retval) {
  char path[PATH_MAX];
  copyinstr(pathname, (void*)&path, sizeof(path), NULL);
  int result = vfs_mkdir(path, mode);
  *retval = result;
  return result;
}

int sys_rmdir(userptr_t pathname, int *retval) {
  char path[PATH_MAX];
  copyinstr(pathname, (void*)&path, sizeof(path), NULL);
  int result = vfs_rmdir(path);
  *retval = result;
  return result;
}

// get next filename in directory fd
int sys_getdirentry(int fd, userptr_t dirname_ubuf, size_t buf_count, int *retval) {
  if (!file_is_open(fd)) {
    *retval = -1;
    return EBADF;
  }
  if (!file_is_dir(fd)) {
    *retval = -1;
    return ENOTDIR;
  }
  struct filedes *file_des = filetable_get(curproc, fd);
  KASSERT(file_des);
  if (buf_count > PATH_MAX) {
    buf_count = PATH_MAX;
  }
  struct iovec iov;
  struct uio myuio;
  uio_uinit(&iov, &myuio, dirname_ubuf, buf_count, file_des->offset, UIO_READ);
  int res = VOP_GETDIRENTRY(file_des->node, &myuio);
  if (res != 0) {
    *retval = -1;
    return res;
  }
  if (myuio.uio_offset == 0) { // no more entries
    *retval = 0;
    return 0;
  }
  file_des->offset += 1; // this is the offset to the next entry in the directory (the "slot")
  *retval = buf_count - myuio.uio_resid; // amount of bytes written to dirname_ubuf
  return 0;
}
