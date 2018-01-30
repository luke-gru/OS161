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
#include <kern/errno.h>
#include <stat.h>
#include <kern/seek.h>
#include <kern/fcntl.h>
#include <uio.h>
#include <syscall.h>

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
static struct filedes *filedes_create(struct proc *p, char *pathname, struct vnode *node, int flags, int table_idx) {
	struct filedes *file_des = kmalloc(sizeof(*file_des));
	file_des->pathname = kstrdup(pathname);
	file_des->node = node;
	file_des->flags = flags;
	file_des->offset = 0;
	file_des->refcount = 1;
	file_des->lk = lock_create("file lock");
	file_des->latest_fd = -1;
	int fd = filetable_put(p, file_des, table_idx);
	// TODO: return error when too many files being opened
	KASSERT(fd != -1);
	file_des->latest_fd = fd;
	return file_des;
}
static void filedes_destroy(struct proc *p, struct filedes *file_des) {
	kfree(file_des->pathname);
	file_des->flags = 0; file_des->offset = 0;
	KASSERT(!lock_do_i_hold(file_des->lk));
	lock_destroy(file_des->lk);
	vfs_close(file_des->node); // check success?
	file_des->node = (void*)0xdeadbeef;
	filetable_nullout(p, file_des);
	file_des->refcount = 0;
	kfree(file_des);
}

// close file descriptor for current process.
void filedes_close(struct proc *p, struct filedes *file_des) {
	KASSERT(file_des);
	if (!p) p = curproc;
	DEBUGASSERT(file_des->refcount > 0);
	if (file_des->refcount > 0) {
		file_des->refcount--;
	}
	if (file_des->refcount <= 0) {
		filedes_destroy(p, file_des);
	}
}

struct filedes *filedes_open(struct proc *p, char *pathname, struct vnode *node, int flags, int table_idx) {
	if (!p) p = curproc;
	return filedes_create(p, pathname, node, flags, table_idx);
}

static void filedes_inherit(struct proc *p, struct filedes *file_des, int idx) {
	DEBUGASSERT(idx >= 0);
	file_des->refcount++;
	int put_res = filetable_put(p, file_des, idx);
	DEBUGASSERT(put_res != -1);
}

int filetable_put(struct proc *p, struct filedes *fd, int idx) {
	if (!p) p = curproc;
	struct filedes **fd_tbl = p->file_table;
	// add or clear a fd from the table, given an index
	if (idx >= 0) {
		fd_tbl[idx] = fd; // NOTE: can be NULL
		return idx;
	} else if (idx == -1) { // find first non-NULL index and add the fd there
		for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
			if (fd_tbl[i] == NULL) {
				fd_tbl[i] = fd;
				return i;
			}
		}
	} else {
		panic("wrong value to filetable_put");
	}
	return -1;
}

struct filedes *filetable_get(struct proc *p, int fd) {
	if (fd < 0 || fd >= FILE_TABLE_LIMIT) {
		return NULL;
	}
	if (!p) p = curproc;
	return p->file_table[fd];
}

int filetable_nullout(struct proc *p, struct filedes *des) {
	if (!p) p = curproc;
	struct filedes **fd_tbl = p->file_table;
	int num_nulled = 0;
	if (!des) return -1;
	for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
		if (fd_tbl[i] == des) {
			fd_tbl[i] = NULL;
			num_nulled++;
		}
	}
	return num_nulled;
}

int filetable_find_first_fd(struct proc *p, struct filedes *des) {
	if (!p) p = curproc;
	struct filedes **fd_tbl = p->file_table;
	if (!des) return -1;

	for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
		if (fd_tbl[i] == des) {
			return i;
		}
	}
	return -1;
}

bool filedes_is_open(struct filedes *file_des) {
	return filetable_find_first_fd(curproc, file_des) != -1;
}
bool filedes_is_readable(struct filedes *file_des) {
	KASSERT(file_des);
	return (file_des->flags & O_ACCMODE) != O_WRONLY;
}
bool filedes_is_writable(struct filedes *file_des) {
	KASSERT(file_des);
	return (file_des->flags & O_ACCMODE) != O_RDONLY;
}
bool filedes_is_device(struct filedes *file_des) {
	KASSERT(file_des);
	return vnode_is_device(file_des->node);
}
bool filedes_is_seekable(struct filedes *file_des) {
	KASSERT(file_des);
	return VOP_ISSEEKABLE(file_des->node);
}
bool filedes_is_console(struct filedes *file_des) {
	return filedes_is_device(file_des);
}

off_t filedes_size(struct filedes *file_des, int *errcode) {
	struct stat st;
	int res = filedes_stat(file_des, &st, errcode);
	if (res != 0) {
		return res; // errcode set above
	}
	return st.st_size;
}

int filedes_stat(struct filedes *file_des, struct stat *st, int *errcode) {
	KASSERT(file_des);
	int res = VOP_STAT(file_des->node, st); // fills out stat struct
	if (res != 0) {
		*errcode = res;
		return -1;
	}
	return 0;
}

bool file_is_open(int fd) {
	return filetable_get(curproc, fd) != NULL;
}

bool file_is_readable(char *path) {
	(void)path;
	return true; // TODO
}

bool file_is_writable(char *path) {
	(void)path;
	return true; // TODO
}

// Non-zero return value is error
int file_close(int fd) {
	struct filedes *file_des = filetable_get(curproc, fd);
	if (!file_des) {
		return EBADF;
	}
	filedes_close(curproc, file_des);
	int put_res = filetable_put(curproc, NULL, fd);
	if (put_res < 0) {
		return -1;
	}
	return 0;
}

/*
	Returns 0 on success, otherwise returns error code.
	Removes the named file from the current process's filetable. TODO: If child
	processes have it open, then it doesn't remove it right away, but does so
	when it's subsequently closed for the last time (filedes->refcount == 0).
*/
int file_unlink(char *path) {
	if (!file_exists(path)) {
		return EBADF;
	}
	int res = vfs_remove(path);
	return res;
}

// Does the file exist on the mounted filesystem?
bool file_exists(char *path) {
	struct vnode *fnode;
	int res = vfs_lookup(path, &fnode);
	if (res != 0) {
		return false;
	}
	VOP_DECREF(fnode);
	return true;
}

bool file_is_dir(int fd) {
	struct filedes *file_des = filetable_get(curproc, fd);
	if (!file_des) return false;
	struct stat st;
	int errcode;
	int res = filedes_stat(file_des, &st, &errcode);
	if (res != 0) {
		return false; // TODO: propagate error? add param *errcode?
	}
	return S_ISDIR(st.st_mode);
}

// Try opening or creating the file, returning fd > 0 on success. On error, *retval is set
// to a non-zero error code. On success, adds filedes to current process's file table.
int file_open(char *path, int openflags, mode_t mode, int *errcode) {
	struct vnode *node;
	int result = vfs_open(path, openflags, mode, &node);
	if (result != 0) {
		//panic("FILE ALREADY OPEN"); // FIXME
		*errcode = result;
		return -1;
	}
	struct filedes *new_filedes = filedes_open(curproc, path, node, openflags, -1);
	if (!new_filedes) {
		*errcode = EMFILE; // too many file descriptors for process
		return -1;
	}
	if (filedes_is_writable(new_filedes) && ((openflags & O_APPEND) != 0)) {
		result = file_seek(new_filedes, 0, SEEK_END, errcode);
		if (result != 0) {
			filedes_close(curproc, new_filedes); // can't seek to end, so close file
			// errcode set in file_seek
			return -1;
		}
	}
	// TODO: implement O_TRUNC when file is writable
	return new_filedes->latest_fd;
}

int file_read(struct filedes *file_des, struct uio *io, int *errcode) {
	if (!filedes_is_device(file_des) && !filedes_is_readable(file_des)) {
    *errcode = EBADF;
		return -1;
  }
	if (!lock_do_i_hold(file_des->lk))
		lock_acquire(file_des->lk);
	// NOTE: this must be before VOP_READ, as it's modified by the operation
	int count = io->uio_iov->iov_len;
	int res = VOP_READ(file_des->node, io); // 0 on success
  if (res != 0) {
		*errcode = EIO;
		lock_release(file_des->lk);
    return -1;
  }
  int bytes_read = count - io->uio_resid;
	if (bytes_read > 0)
 		file_des->offset = io->uio_offset; // update file offset

	lock_release(file_des->lk);
  return bytes_read;
}

// Returns number of bytes written on success, and -1 on error with *errcode
// set
int file_write(struct filedes *file_des, struct uio *io, int *errcode) {
	if (!file_des || !filedes_is_writable(file_des)) {
		*errcode = EBADF;
		return -1;
	}
  int res = 0;
	if (!lock_do_i_hold(file_des->lk))
		lock_acquire(file_des->lk);
	// NOTE: this must be before VOP_WRITE, as it's modified by the operation
	int count = io->uio_iov->iov_len;
  res = VOP_WRITE(file_des->node, io);
  if (res != 0) {
		*errcode = res;
		lock_release(file_des->lk);
		return -1;
  }
	int bytes_written = count - io->uio_resid;
	if (bytes_written != count) {
		panic("invalid write in file_write: %d", bytes_written); // FIXME
	}
	file_des->offset = io->uio_offset;
	lock_release(file_des->lk);
	return bytes_written;
}

// FIXME: lock appropriately
// man 2 lseek for more info
int file_seek(struct filedes *file_des, int32_t offset, int whence, int *errcode) {
	if (!file_des || !filedes_is_seekable(file_des)) {
		*errcode = EBADF;
		return -1;
	}
	int32_t new_offset = offset;
	int32_t cur_size;
	switch(whence) {
  case SEEK_SET:
		new_offset = offset;
		break;
	case SEEK_END:
		/* $ man 2 lseek
		TODO:
		The lseek() function allows the file offset to be set beyond the end of
		the file (but this does not change the size of the file).  If  data  is
		later written at this point, subsequent reads of the data in the gap (a
		"hole") return null bytes ('\0') until data is  actually  written  into
		 the gap.
		*/
		cur_size = filedes_size(file_des, errcode);
		if (cur_size == -1) {
			return -1; // errcode set above
		}
	  new_offset = cur_size + offset;
		break;
	case SEEK_CUR:
		new_offset = file_des->offset + offset;
		break;
	default:
		*errcode = EINVAL;
		return -1;
	}

	if (new_offset < 0) {
		*errcode = EINVAL;
		return -1;
	}
	DEBUGASSERT(file_des->offset >= 0);
	file_des->offset = new_offset;
	return 0;
}

/*
 * Create a proc structure with empty address space and file table.
 */
static struct proc *proc_create(const char *name) {
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
	bzero((void *)proc->file_table, FILE_TABLE_LIMIT * sizeof(struct filedes*));
	proc->p_mutex = lock_create("proc mutex");

	return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: You can't destroy the currently running process, this is only callable
 * from other processes.
 */
void proc_destroy(struct proc *proc) {
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);
	KASSERT(proc != curproc);

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
	for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
		struct filedes *file_des;
		if ((file_des = filetable_get(proc, i)) != NULL) {
			filedes_close(proc, file_des);
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
	// TODO: lock userprocs access
	for (int i = 0; i < MAX_USERPROCS; i++) {
		p = userprocs[i];
		if (p != NULL && p->pid == proc->pid) {
			userprocs[i] = NULL;
		}
	}

	kfree(proc->p_name);
	lock_destroy(proc->p_mutex);
	bzero((void *)proc->file_table, FILE_TABLE_LIMIT * sizeof(struct filedes*));
	kfree(proc);
}

int proc_fork(struct proc *parent_pr, struct thread *parent_th, struct trapframe *tf, int *err) {
	KASSERT(is_current_userspace_proc(parent_pr));
	struct proc *child_pr = NULL;
	child_pr = proc_create(parent_pr->p_name);
	if (!child_pr) {
		*err = ENOMEM;
		return -1;
	}
	child_pr->p_parent = parent_pr;
	child_pr->p_addrspace = NULL;
	lock_acquire(parent_pr->p_mutex);
	int res = as_copy(proc_getas(), &child_pr->p_addrspace); // NOTE assumes parent_pr == curproc
	if (res != 0) {
		lock_release(parent_pr->p_mutex);
		*err = res;
		return -1;
	}
	KASSERT(child_pr->p_addrspace != NULL);
	res = proc_inherit_filetable(parent_pr, child_pr);
	if (res != 0) {
		lock_release(parent_pr->p_mutex);
		*err = res;
		return -1;
	}
	if (parent_pr->p_cwd != NULL) {
		VOP_INCREF(parent_pr->p_cwd);
		child_pr->p_cwd = parent_pr->p_cwd;
	}
	int fork_errcode = 0;
	res = thread_fork_from_proc(
		parent_th,
		child_pr,
		tf,
		&fork_errcode
	);
	if (res < 0) {
		*err = fork_errcode;
		lock_release(parent_pr->p_mutex);
		proc_destroy(child_pr);
		return -1;
	} else {
		KASSERT(child_pr->pid > 0);
		lock_release(parent_pr->p_mutex);
		return child_pr->pid;
	}
}

int proc_pre_exec(struct proc *p, char *progname) {
	spinlock_acquire(&p->p_lock);
	if (p->p_name)
		kfree(p->p_name);
	p->p_name = kstrdup(progname);
	p->p_numthreads = 1;
	spinlock_release(&p->p_lock);
	return 0;
}

// TODO: lock userprocs access
unsigned proc_numprocs(void) {
	unsigned num = 0;
	struct proc *p = NULL;
	for (int i = 0; i < MAX_USERPROCS; i++) {
		p = userprocs[i];
		if (p != NULL && p->pid != INVALID_PID) {
			num++;
		}
	}
	return num;
}

/*
 * Create the process structure for the kernel.
 */
void proc_bootstrap(void) {
	kproc = proc_create("[kernel]");
	kproc->pid = BOOTUP_PID;
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
	for (int i = 0; i < MAX_USERPROCS; i++) {
		userprocs[i] = NULL;
	}
	pid_bootstrap();
}

/*
 * Initialize process file table to hold only STDIN, STDOUT, STDERR
 */
int proc_init_filetable(struct proc *p) {
	struct vnode *console_in = NULL;
	struct vnode *console_out = NULL;
	struct vnode *console_err = NULL;
	int console_result = 0;
	const char *console_name = "con:";
	console_result = vfs_open(kstrdup(console_name), O_RDONLY, 0, &console_in);
	if (console_result != 0) {
		panic("couldn't open console_in. Result: %d", console_result); // FIXME
	}
	console_result = vfs_open(kstrdup(console_name), O_WRONLY, 0, &console_out);
	if (console_result != 0) {
		panic("couldn't open console_out. Result: %d", console_result); // FIXME
	}
	console_result = vfs_open(kstrdup(console_name), O_WRONLY, 0, &console_err);
	if (console_result != 0) {
		panic("couldn't open console_err. Result: %d", console_result); // FIXME
	}
	KASSERT(console_in != NULL);
	KASSERT(console_out != NULL);
	KASSERT(console_err != NULL);
	filedes_open(
		p,
		(char*)special_filedes_name(0),
		console_in,
		special_filedes_flags(0),
		0
	);
	filedes_open(
		p,
		(char*)special_filedes_name(1),
		console_out,
		special_filedes_flags(1),
		1
	);
	filedes_open(
		p,
		(char*)special_filedes_name(2),
		console_err,
		special_filedes_flags(2),
		2
	);
	return 0;
}

int proc_inherit_filetable(struct proc *parent, struct proc *child) {
	struct filedes *fd = NULL;
	for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
		fd = filetable_get(parent, i);
		if (fd) {
			filedes_inherit(child, fd, i);
		}
	}
	return 0;
}

void proc_close_filetable(struct proc *p, bool include_std_streams) {
	struct filedes *fd = NULL;
	for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
		fd = filetable_get(p, i);
		if (fd) {
			if ((i < 3 && include_std_streams) || i >= 3) {
				filedes_close(p, fd);
			}
		}
	}
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
// TODO: use hash table instead of linear array
struct proc *proc_lookup(pid_t pid) {
	struct proc *found = NULL;
	// TODO: lock userprocs access
	for (int i = 0; i < MAX_USERPROCS; i++) {
		found = userprocs[i];
		if (found && found->pid == pid) {
			return found;
		}
	}
	return NULL;
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *proc_create_runprogram(const char *name) {
	int res = 0;
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
	res = proc_init_filetable(newproc);
	if (res != 0) {
		proc_destroy(newproc);
		return NULL;
	}

	// the pid is generated in thread_fork if thread_fork is passed a userspace process
	newproc->pid = INVALID_PID;

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
	t->t_pid = proc->pid;
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
struct addrspace *proc_getas(void) {
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
struct addrspace *proc_setas(struct addrspace *newas) {
	struct addrspace *oldas;
	struct proc *p = curproc;

	KASSERT(p != NULL);
	KASSERT(p != kproc);

	spinlock_acquire(&p->p_lock);
	oldas = p->p_addrspace;
	p->p_addrspace = newas;
	spinlock_release(&p->p_lock);
	return oldas;
}

int proc_waitpid_sleep(pid_t child_pid, int *errcode) {
	KASSERT_CAN_SLEEP();
	int status = -1001; // should be set below
	DEBUG(DB_SYSCALL, "waiting on process %d\n", (int)child_pid);
	int res = pid_wait_sleep(child_pid, &status);
	if (res != 0) {
		DEBUG(DB_SYSCALL, "waiting on process failed, error: %d\n", res);
		*errcode = res;
		return -1;
	}
	DEBUG(DB_SYSCALL, "done waiting on process %d, exitstatus %d\n", (int)child_pid, status);
	exorcise();
	struct proc *child = proc_lookup(child_pid);
	if (child) {
		panic("child should have been exorcised!");
		return -1;
	}
	return status; // exitstatus
}

bool is_current_userspace_proc(struct proc *p) {
	return is_valid_user_pid(p->pid);
}
