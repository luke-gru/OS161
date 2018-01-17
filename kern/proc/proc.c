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

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <vfs.h>
#include <pid.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

const char *special_filedes_name(int i) {
	switch (i) {
		case 0:
			return "STDIN";
		case 1:
			return "STDOUT";
		case 2:
			return "STDERR";
		default:
			panic("invalid special filedes: %d", i);
			return "";
	}
}
int special_filedes_flags(int i) {
	switch (i) {
		case 0:
			return O_RDONLY;
		case 1:
			return O_WRONLY;
		case 2:
			return O_WRONLY;
		default:
			panic("invalid special filedes: %d", i);
			return -1;
	}
}
struct filedes *filedes_create(char *pathname, struct vnode *node, int flags) {
	struct filedes *file_des = kmalloc(sizeof(*file_des));
	file_des->pathname = kstrdup(pathname);
	file_des->node = node;
	file_des->flags = flags;
	file_des->offset = 0;
	return file_des;
}
void filedes_destroy(struct filedes *file_des) {
	kfree(file_des->pathname);
	VOP_DECREF(file_des->node);
	kfree(file_des);
}

bool file_is_open(struct filedes *file_des) {
	return file_des != NULL;
}
bool file_is_readable(struct filedes *file_des) {
	KASSERT(file_des);
	return (file_des->flags & O_RDONLY) != 0 ||
		(file_des->flags & O_RDWR) != 0;
}
bool file_is_writable(struct filedes *file_des) {
	KASSERT(file_des);
	return (file_des->flags & O_WRONLY) != 0 ||
		(file_des->flags & O_RDWR) != 0;
}
bool file_is_device(struct filedes *file_des) {
	KASSERT(file_des);
	return vnode_is_device(file_des->node);
}

/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	proc->p_parent = NULL;

	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	proc->p_numthreads = 0;
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	proc->pid = INVALID_PID;
	bzero((void *)proc->file_table, FILE_TABLE_LIMIT);

	return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}
	// TODO: decrement reference, don't NULL out (for fork)
	for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
		if (proc->file_table[i] != NULL) {
			filedes_destroy(proc->file_table[i]);
			proc->file_table[i] = NULL;
		}
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);

	struct proc *p;
	for (int i = 0; i < MAX_USERPROCS; i++) {
		p = userprocs[i];
		if (p != NULL && p->pid == proc->pid) {
			userprocs[i] = NULL;
		}
	}

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
	for (int i = 0; i < MAX_USERPROCS; i++) {
		userprocs[i] = NULL;
	}
	pid_bootstrap();
}

int proc_init_filetable(struct proc *p) {
	struct filedes **table = p->file_table;
	struct vnode *console = NULL;
	int console_result = 0;
	for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
		if (i == 0 || i == 1 || i == 2) {
			if (console == NULL) {
				const char *console_name = "con:";
				console_result = vfs_lookup(kstrdup(console_name), &console);
				if (console_result != 0) {
					panic("couldn't grab console vnode! Result: %d", console_result); // FIXME
				}
				KASSERT(console != NULL);
			}
			struct filedes *console_filedes = filedes_create(
				(char *)special_filedes_name(i),
				console,
				special_filedes_flags(i)
			);
			KASSERT(console_filedes);
			table[i] = console_filedes; // returns vnode* for the console device
		} else {
			table[i] = NULL; // FIXME: initialize from parent on fork
		}
	}
	return 0;
}

/*
 * create new pid for process, and adds process to `userprocs` array
 */
int proc_init_pid(struct proc *p) {
	for (int i = 0; i < MAX_USERPROCS; i++) {
		if (userprocs[i] == NULL) {
			pid_t new_pid;
			int result = pid_alloc(&new_pid);
			if (result != 0) {
				return result; // error
			}
			p->pid = new_pid;
			userprocs[i] = p;
			return 0;
		}
	}
	return -1;
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;
	struct proc *parent_proc = NULL;
	if (curproc != kproc) {
		parent_proc = curproc;
	}

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}
	newproc->p_parent = parent_proc;

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */
	KASSERT(proc_init_filetable(newproc) == 0);

	// pid is initialized in thread_fork if thread_fork is passed a userspace process
	newproc->pid = 0;

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	return newproc;
}


/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL || proc == kproc) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}

int
proc_waitpid(pid_t child_pid) {
	int status;
	kprintf("waiting on process %d\n", (int)child_pid);
	int res = pid_wait(child_pid, &status);
	kprintf("done waiting on process %d\n", (int)child_pid);
	if (res != 0) {
		return res;
	}
	return status;
}
