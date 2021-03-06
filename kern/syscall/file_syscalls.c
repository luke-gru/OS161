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
#include <kern/fcntl.h>
#include <current.h>
#include <vnode.h>
#include <spl.h>
#include <kern/select.h>
#include <kern/time.h>

int sys_chdir(userptr_t dirbuf, int *retval) {
  char fname[PATH_MAX];
  size_t gotlen;
  int copy_res = copyinstr(dirbuf, fname, sizeof(fname), &gotlen);
  if (copy_res != 0) {
    *retval = -1;
    return copy_res;
  }
  if (gotlen <= 1) {
    *retval = -1;
    return EINVAL;
  }
  int res = vfs_chdir(fname); // sets curproc->p_pwd on success
  if (res != 0) {
    DEBUG(DB_SYSCALL, "Error in sys_chdir: process %d: code=%d (%s)\n",
      curproc->pid, res, strerror(res));
    *retval = -1;
    return res;
  }
  *retval = 0;
  return 0;
}

int sys_close(int fd, int *retval) {
  struct filedes *file_des = filetable_get(curproc, fd);
  if (!file_des) {
    *retval = -1;
    return EBADF;
  }
  if (file_des->ftype == FILEDES_TYPE_PIPE && file_des->pipe->is_writer) {
    // notify reader if it's blocked
    if (!file_des->pipe->pair->is_closed && file_des->pipe->pair->buflen > 0) {
      file_des->pipe->is_closed = true;
      DEBUG(DB_SYSCALL, "Closed pipe writer, signalling reader\n");
      pipe_signal_can_read(file_des->pipe->pair);
    }
  }
  int res = file_close(fd);
  if (res != 0) {
    DEBUG(DB_SYSCALL, "Error in sys_close: fd %d for process %d: code=%d (%s)\n",
      fd, curproc->pid, res, strerror(res));
    *retval = -1;
    return res;
  }
  *retval = 0;
  return 0;
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
  int copy_res = copyout(&st, stat_buf, sizeof(struct stat));
  if (copy_res != 0) {
    DEBUG(DB_SYSCALL, "Error in sys_fstat: user stat_buf invalid\n");
    *retval = -1;
    return copy_res;
  }
  *retval = 0;
  return 0;
}

int sys_fcntl(int fd, int cmd, int flags, int *retval) {
  int errcode = 0;
  struct filedes *file_des = filetable_get(curproc, fd);
  if (!file_des) {
    DEBUG(DB_SYSCALL, "Error in sys_fcntl: fd %d for process %d: file not open\n",
      fd, curproc->pid);
    *retval = -1;
    return EBADF;
  }
  int res = filedes_fcntl(file_des, cmd, flags, &errcode);
  if (res == -1) {
    *retval = -1;
    return errcode;
  } else {
    *retval = res;
    return 0;
  }
}

int sys_access(userptr_t pathname, int mode, int *retval) {
  if (pathname == (userptr_t)0) {
    *retval = -1;
    return EFAULT;
  }
  char path[PATH_MAX];
  size_t gotlen;
  int copy_res = copyinstr(pathname, path, sizeof(path), &gotlen);
  if (copy_res != 0 || gotlen <= 1) {
    *retval = -1;
    if (copy_res == 0) { // 0-length pathname
      return ENOENT;
    } else {
      return copy_res;
    }
  }
  int access_err = 0;
  int access_res = file_access(path, mode, &access_err);
  if (access_res == 0) {
    *retval = 0;
    return 0;
  } else {
    *retval = -1;
    return access_err;
  }
}

int sys_lseek(int fd, int32_t offset, int whence, int *retval) {
  struct filedes *file_des = filetable_get(curproc, fd);
  if (!file_des) {
    DEBUG(DB_SYSCALL, "Error in sys_lseek: fd %d for process %d, file not open\n",
      fd, curproc->pid);
    *retval = -1;
    return EBADF;
  }
  int errcode = 0;
  int res = file_seek(file_des, offset, whence, &errcode);
  if (res != 0 || errcode != 0) {
    DEBUG(DB_SYSCALL, "Error in sys_lseek: fd %d for process %d, code=%d, err=%s\n",
      fd, curproc->pid, errcode, strerror(errcode));
    *retval = -1;
    return errcode;
  }
  DEBUGASSERT(file_des->offset == offset);
  *retval = file_des->offset;
  return 0;
}

int sys_remove(userptr_t filename, int *retval) {
  char path[PATH_MAX];
  size_t gotlen;
  int copy_res = copyinstr(filename, (void*)&path, sizeof(path), &gotlen);
  if (copy_res != 0 || gotlen <= 1) {
    *retval = -1;
    if (copy_res == 0) { // empty filename
      return EINVAL;
    } else {
      return copy_res;
    }
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
  int spl = splhigh();
  struct filedes *file_des = filetable_get(curproc, oldfd);
  if (!file_des) {
    DEBUG(DB_SYSCALL, "Error in sys_dup2: oldfd %d for process %d: file not open\n",
      oldfd, curproc->pid);
    *retval = -1;
    splx(spl);
    return EBADF;
  }
  if (newfd < 0 || newfd >= FILE_TABLE_LIMIT || oldfd == newfd) {
    DEBUG(DB_SYSCALL, "Error in sys_dup2: newfd invalid: %d\n", newfd);
    *retval = -1;
    splx(spl);
    return EBADF;
  }
  lock_acquire(file_des->lk);
  struct filedes *replaced = filetable_get(curproc, newfd);
  if (replaced) {
    filedes_close(curproc, replaced);
  }
  int newfd_res = filetable_put(curproc, file_des, newfd);
  if (newfd_res < 0) {
    DEBUG(DB_SYSCALL, "Error in sys_dup2: replacement failed: (old: %d, new: %d)\n",
      oldfd, newfd);
      *retval = -1;
      lock_release(file_des->lk);
      splx(spl);
      return EMFILE;
  }
  file_des->refcount++;
  *retval = newfd;
  lock_release(file_des->lk);
  splx(spl);
  DEBUG(DB_SYSCALL, "dup2: proc %d, %d => %d\n", curproc->pid, oldfd, newfd);
  return 0;
}

int sys_flock(int fd, int op, int *retval) {
  struct filedes *file_des = filetable_get(curproc, fd);
  if (!file_des) {
    DEBUG(DB_SYSCALL, "Error in sys_flock: file not open\n");
    *retval = -1;
    return EBADF;
  }
  int lock_res = file_flock(file_des, op);
  if (lock_res != 0) {
    *retval = -1;
    DEBUG(DB_SYSCALL, "Error in sys_flock (file_flock): %d (%s)\n", lock_res, strerror(lock_res));
    return lock_res;
  }
  *retval = 0;
  return 0;
}

int sys_mkdir(userptr_t pathname, mode_t mode, int *retval) {
  char path[PATH_MAX];
  size_t gotlen;
  int copy_res = copyinstr(pathname, path, sizeof(path), &gotlen);
  if (copy_res != 0 || gotlen <= 1) {
    *retval = -1;
    if (copy_res == 0) {
      return EINVAL;
    } else {
      return copy_res;
    }
  }
  int result = vfs_mkdir(path, mode);
  *retval = result;
  return result;
}

int sys_rmdir(userptr_t pathname, int *retval) {
  char path[PATH_MAX];
  size_t gotlen;
  int copy_res = copyinstr(pathname, path, sizeof(path), &gotlen);
  if (copy_res != 0 || gotlen <= 1) {
    *retval = -1;
    if (copy_res == 0) {
      return EINVAL;
    } else {
      return copy_res;
    }
  }
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

int sys_pipe(userptr_t pipes_ary, size_t buflen, int *retval) {
  int reader_fd, writer_fd;
  int pipe_creat_res = file_create_pipe_pair(&reader_fd, &writer_fd, buflen);
  if (pipe_creat_res != 0) {
    *retval = -1;
    return pipe_creat_res;
  }
  int copy_res;
  copy_res = copyout(&reader_fd, pipes_ary, sizeof(int));
  if (copy_res != 0) {
    *retval = -1;
    return copy_res;
  }
  copy_res = copyout(&writer_fd, pipes_ary + sizeof(int), sizeof(int));
  if (copy_res != 0) {
    *retval = -1;
    return copy_res;
  }
  *retval = 0;
  return 0;
}

// NOTE: nfds is the highest numbered FD + 1, but we don't use this optimization because our fd_set is
// quite stupid and right now only allows 10 descriptors to be watched per set.
int sys_select(int nfds, userptr_t readfds, userptr_t writefds,
               userptr_t exceptfds, userptr_t timeval_struct, int *retval) {
  struct fd_set reads, writes, exceptions;
  FD_ZERO(&reads);
  FD_ZERO(&writes);
  FD_ZERO(&exceptions);
  struct timeval timeout;
  struct timeval *timeout_p;
  int copy_res;
  if (nfds <= 0) { // we don't support this, just use nanosleep() for sleeping instead of select(0,0,0,0,0)
    *retval = -1;
    return EINVAL;
  }
  if (readfds != (userptr_t)0) {
    DEBUG(DB_SYSCALL, "Copying in readfds for sys_select\n");
    copy_res = copyin(readfds, &reads, sizeof(struct fd_set));
    DEBUGASSERT(copy_res == 0);
  }
  if (writefds != (userptr_t)0) {
    DEBUG(DB_SYSCALL, "Copying in writefds for sys_select\n");
    copy_res = copyin(writefds, &writes, sizeof(struct fd_set));
    DEBUGASSERT(copy_res == 0);
  }
  if (exceptfds != (userptr_t)0) {
    DEBUG(DB_SYSCALL, "Copying in exceptfds for sys_select\n");
    copy_res = copyin(exceptfds, &exceptions, sizeof(struct fd_set));
    DEBUGASSERT(copy_res == 0);
  }
  if (timeval_struct != (userptr_t)0) {
    copy_res = copyin(timeval_struct, &timeout, sizeof(struct timeval));
    DEBUGASSERT(copy_res == 0);
    timeout_p = &timeout;
  } else {
    timeout_p = NULL;
  }
  int errcode = 0;
  int num_ready = 0;
  // NOTE: this call can block
  int select_res = file_select((unsigned)nfds, &reads, &writes, &exceptions, timeout_p, &errcode, &num_ready);
  if (select_res != 0) {
    DEBUG(DB_SYSCALL, "file_select failed: %d (%s)\n", errcode, strerror(errcode));
    *retval = -1;
    return errcode;
  }
  if (readfds != (userptr_t)0) {
    copy_res = copyout(&reads, readfds, sizeof(struct fd_set));
    DEBUGASSERT(copy_res == 0);
  }
  if (writefds != (userptr_t)0) {
    copy_res = copyout(&writes, writefds, sizeof(struct fd_set));
    DEBUGASSERT(copy_res == 0);
  }
  if (exceptfds != (userptr_t)0) {
    copy_res = copyout(&exceptions, exceptfds, sizeof(struct fd_set));
    DEBUGASSERT(copy_res == 0);
  }
  *retval = num_ready;
  return 0;
}
