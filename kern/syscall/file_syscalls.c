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

int
sys_chdir(userptr_t dirbuf, int *retval)
{
  char fname[PATH_MAX];
  int copy_res = copyinstr(dirbuf, fname, sizeof(fname), NULL);
  if (copy_res != 0) {
    return copy_res;
  }
  kprintf("dir: '%s'\n", fname);
  if (strcmp(fname, "") == 0) {
    *retval = EINVAL;
    return *retval;
  }
  *retval = vfs_chdir(fname); // sets curproc->p_pwd
  return *retval; // 0 on success
}

int sys_close(int fd, int *retval) {
  int res = file_close(fd);
  *retval = res;
  return res;
}

int sys_fstat(int fd, userptr_t stat_buf, int *retval) {
  (void)fd;
  struct stat st;
  // TODO: get info from VFS
  st.st_size = 0;
  st.st_mode = 0;
  st.st_nlink = 0;
  st.st_blocks = 0;
  st.st_dev = 0;
  st.st_ino = 0;
  st.st_rdev = 0;
  st.st_atime = 0; st.st_ctime = 0; st.st_mtime = 0;
  st.st_atimensec = 0; st.st_ctimensec = 0; st.st_mtimensec = 0;
  st.st_uid = 0;
  st.st_gid = 0;
  st.st_gen = 0;
  st.st_blksize = 0;
  copyout(&st, stat_buf, sizeof(struct stat));
  *retval = 0;
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
