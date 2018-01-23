/*
 * Copyright (c) 2013
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

#ifndef _PROC_H_
#define _PROC_H_

/*
 * Definition of a process.
 *
 * Note: curproc is defined by <current.h>.
 */

#include <spinlock.h>
#include <kern/fcntl.h>
#include <vfs.h>
#include <uio.h>
#include <lib.h>
#include <pid.h>

struct addrspace;
struct thread;
struct vnode;
struct proc;
struct stat;

#define FPATH_MAX 1024
// TODO: move to filedes.c/filedes.h
// Open file descriptor
struct filedes {
	char *pathname;
	struct vnode *node;
	int flags;
	size_t offset;
	int ft_idx; // index into file table of current process
	//int refcount; // filedes may be shared by multiple processes
};

const char *special_filedes_name(int fd);
int special_filedes_flags(int fd);

struct filedes *filetable_get(struct proc *p, int fd);
int filetable_put(struct proc *p, struct filedes *file_des, int idx);

struct filedes *filedes_open(struct proc *p, char *pathname, struct vnode *node, int flags, int table_idx);
void filedes_close(struct proc *p, struct filedes *file_des);
struct filedes *filedes_dup(struct proc *p, struct filedes *file_des, int ft_idx);

off_t filedes_size(struct filedes *file_des, int *errcode);
int filedes_stat(struct filedes *file_des, struct stat *st, int *errcode);

bool filedes_is_open(struct filedes *file_des);
bool filedes_is_writable(struct filedes *file_des);
bool filedes_is_device(struct filedes *file_des);
bool filedes_is_readable(struct filedes *file_des);
bool filedes_is_seekable(struct filedes *file_des);

// NOTE: takes a fd int because a file can be open more than once in a process, returning different file descriptors to the underlying file
bool file_is_open(int fd);
bool file_is_readable(char *path);
bool file_is_writable(char *path);
int  file_close(int fd);
bool file_exists(char *path);
bool file_is_dir(int fd);
struct filedes *file_open(char *path, int openflags, mode_t mode, int *errcode);
int file_write(struct filedes *file_des, struct uio *io, int *errcode);
int file_read(struct filedes *file_des, struct uio *io, int *errcode);
int file_seek(struct filedes *file_des, off_t offset, int whence, int *errcode);

/*
 * Process structure.
 *
 * Note that we only count the number of threads in each process.
 * (And, unless you implement multithreaded user processes, this
 * number will not exceed 1 except in kproc.) If you want to know
 * exactly which threads are in the process, e.g. for debugging, add
 * an array and a sleeplock to protect it. (You can't use a spinlock
 * to protect an array because arrays need to be able to call
 * kmalloc.)
 *
 * You will most likely be adding stuff to this structure, so you may
 * find you need a sleeplock in here for other reasons as well.
 * However, note that p_addrspace must be protected by a spinlock:
 * thread_switch needs to be able to fetch the current address space
 * without sleeping.
 */
#define FILE_TABLE_LIMIT 1024
#define MAX_USERPROCS 64
struct proc *userprocs[MAX_USERPROCS]; // current userspace processes

struct proc {
	char *p_name;			/* Name of this process */
	struct spinlock p_lock;		/* Lock for this structure */
	unsigned p_numthreads;		/* Number of threads in this process */

	pid_t pid; // set on thread_fork
	struct proc *p_parent;
	/* VM */
	struct addrspace *p_addrspace;	/* virtual address space */

	/* VFS */
	struct vnode *p_cwd;		/* current working directory */
	struct filedes *file_table[FILE_TABLE_LIMIT];
	int next_filedes_idx;

	/* add more material here as needed */
};

/* This is the process structure for the kernel and for kernel-only threads. */
extern struct proc *kproc;

/* Call once during system startup to allocate data structures. */
void proc_bootstrap(void);
int proc_init_pid(struct proc *);
struct proc *proc_lookup(pid_t pid);
int proc_init_filetable(struct proc *);
int proc_inherit_filetable(struct proc *parent, struct proc *child);
inline pid_t proc_ppid(struct proc *p);
inline pid_t proc_ppid(struct proc *p) {
	if (p->p_parent != NULL) {
		return p->p_parent->pid;
	} else {
		return INVALID_PID;
	}
}

unsigned proc_numprocs(void); // number of user processes

/* Create a fresh process for use by runprogram(). */
struct proc *proc_create_runprogram(const char *name);

/* Destroy a process. */
void proc_destroy(struct proc *proc);

/* Attach a thread to a process. Must not already have a process */
int proc_addthread(struct proc *proc, struct thread *t);

/* Detach a thread from its process. */
void proc_remthread(struct thread *t);

// wait on child process to finish, collect its exitstatus and clean it up
// (NOTE: blocks, for use internally in kernel process
int proc_waitpid_sleep(pid_t pid, int *errcode);
// Marks current process as sleeping, queues it on CPU and gets it to wait for child to exit.
// When child exits, process continues in non-interrupt context and returns status to userlevel
// status buffer.
int proc_waitpid_nosleep(pid_t child_pid, userptr_t status_buf, int *errcode);
int proc_fork(struct proc *parent, struct thread *th, int *errcode);

/* Fetch the address space of the current process. */
struct addrspace *proc_getas(void);

/* Change the address space of the current process, and return the old one. */
struct addrspace *proc_setas(struct addrspace *);


#endif /* _PROC_H_ */
