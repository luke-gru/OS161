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

#ifndef _SYSCALL_H_
#define _SYSCALL_H_


#include <cdefs.h> /* for __DEAD */
struct trapframe; /* from <machine/trapframe.h> */
struct thread;
struct argvdata;

/*
 * The system call dispatcher.
 */

void syscall(struct trapframe *tf);

/*
 * Support functions.
 */

/* Helper for fork(). You write this. */
void enter_forked_process(void *data1, unsigned long data2);

/* Enter user mode. Does not return. */
__DEAD void enter_new_process(int argc, userptr_t argv, userptr_t env,
		       vaddr_t stackptr, vaddr_t entrypoint);
__DEAD void enter_cloned_process(void *data1, unsigned long data2);

int runprogram_uspace(char *progname, struct argvdata *argdata, char **environ, int num_env_vars);

/*
 * Prototypes for IN-KERNEL entry points for system call implementations.
 */

int sys_reboot(int code);

int sys_write(int fd, userptr_t buf, size_t count, int *retval);
int sys_open(userptr_t path, int openflags, mode_t mode, int *retval);
int sys_close(int fd, int *retval);
int sys_read(int fd, userptr_t buf, size_t count, int *retval);
int sys_access(userptr_t pathname, int mode, int *retval);
int sys_fstat(int fd, userptr_t stat_buf, int *retval);
int sys_fcntl(int fd, int cmd, int flags, int *retval);
int sys_lseek(int fd, int32_t offset, int whence, int *retval);
int sys_remove(userptr_t filename, int *retval);
int sys_mkdir(userptr_t pathname, mode_t mode, int *retval);
int sys_rmdir(userptr_t pathname, int *retval);
int sys_getdirentry(int fd, userptr_t fname_buf, size_t buf_count, int *retval);
int sys_dup(int fd, int *retval);
int sys_dup2(int oldfd, int newfd, int *retval);

int sys_flock(int fd, int op, int *retval);

int sys_fork(struct trapframe *tf, int *retval);
int sys_vfork(struct trapframe *tf, int *retval);
int sys_waitpid(pid_t child_pid, userptr_t status, int options, int *retval);
void sys_exit(int status);
int sys_execve(userptr_t filename, userptr_t argv, userptr_t envp, int *retval);
int sys_clone(userptr_t func, uint32_t child_stack_top, size_t stacksize, int flags, int *retval);

int sys_sbrk(size_t increment, int *retval);
int sys_mmap(size_t nbytes, int prot, int flags, int fd, off_t offset, int *retval);
int sys_munmap(uint32_t startaddr, int *retval);
int sys_msync(uint32_t startaddr, size_t length, int flags, int *retval);

int sys_chdir(userptr_t dir, int *retval);
int sys_getpid(int *retval);
int sys_getcwd(userptr_t ubuf, size_t ubuf_len, int *retval);

int sys___time(userptr_t user_seconds, userptr_t user_nanoseconds);

int sys_sleep(int seconds, int *retval);
int sys_pipe(userptr_t pipes_ary, size_t buflen, int *retval);
int sys_select(int nfds, userptr_t readfds, userptr_t writefds,
					 userptr_t exceptfds, userptr_t timeval_struct, int *retval);

int sys_socket(int af, int type, int proto, int *retval);
int sys_bind(int sockfd, userptr_t sockaddr_user, socklen_t sockaddr_len, int *retval);
int sys_listen(int sockfd, int backlog, int *retval);
int sys_accept(int sockfd, userptr_t sockaddr_peer, userptr_t sockaddr_peer_len, int *retval);

// NOTE: this is used to test paging memory in/out and memory region locking
// in the kernel!
int sys_pageout_region(uint32_t startaddr, size_t nbytes, int *retval);
int sys_lock_region(uint32_t startaddr, size_t nbytes, int *retval);

#endif /* _SYSCALL_H_ */
