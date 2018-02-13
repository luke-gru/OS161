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
#include <wchan.h>
#include <copyinout.h>
#include <signal.h>
#include <kern/select.h>
#include <clock.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;
struct proc *kswapproc;

static struct filedes *devnull;
// FIXME: io listeners are global right now, not per process or file table
static struct io_pollset *io_listeners[FILE_TABLE_LIMIT];

extern struct wchan *sel_wchan;
extern struct spinlock sel_wchan_lk;

struct flock_node *file_locks;
static struct flock_node *flocklist_virt_head;
static struct flock_node *flocklist_virt_tail;

static const char *default_proc_environ[] = {
	"PATH=/bin:/testbin",
	"SHELL=/bin/sh",
	"TERM=vt220",
	NULL,
};

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
static struct filedes *filedes_create(struct proc *p, char *pathname, struct vnode *node, int flags, int table_idx, int *errcode) {
	struct filedes *file_des = kmalloc(sizeof(*file_des));
	KASSERT(file_des);
	file_des->pathname = kstrdup(pathname);
	file_des->ftype = FILEDES_TYPE_REG;
	file_des->node = node;
	file_des->pipe = NULL;
	file_des->sock = NULL;
	file_des->flags = flags;
	file_des->offset = 0;
	file_des->refcount = 1;
	file_des->flock_nodep = NULL;
	file_des->lk = lock_create("file lock");
	file_des->latest_fd = -1;
	int fd = filetable_put(p, file_des, table_idx);
	if (fd == -1) {
		kfree(file_des->pathname);
		lock_destroy(file_des->lk);
		kfree(file_des);
		*errcode = EMFILE;
		return NULL;
	}
	file_des->latest_fd = fd;
	return file_des;
}
static void filedes_destroy(struct proc *p, struct filedes *file_des) {
	DEBUGASSERT(file_des != devnull);
	if (file_des->ftype == FILEDES_TYPE_PIPE) {
		// other side of pipe is also closed, safe to destruct pipe pair
		if (file_des->pipe->pair && file_des->pipe->pair->is_closed) {
			file_des->pipe->is_closed = true;
			struct pipe *reader, *writer;
			if (file_des->pipe->is_writer) {
				writer = file_des->pipe;
				reader = writer->pair;
			} else {
				reader = file_des->pipe;
				writer = reader->pair;
			}
			pipe_destroy_pair(reader, writer);
			file_des->refcount = 0;
			filetable_nullout(p, file_des);
			kfree(file_des);
			return;
		// gets here if creating the writer pair of the pipe resulted in an error, so we
		// destroy the reader to free the memory
		} else if (file_des->pipe->pair == NULL && !file_des->pipe->is_writer) {
			DEBUG(DB_SYSCALL, "Destroying lone reader side of pipe after creating writer failed\n");
			pipe_destroy_reader(file_des->pipe);
			file_des->refcount = 0;
			filetable_nullout(p, file_des);
			kfree(file_des);
			return;
		// the other side of the pipe is still open, so we don't free this side's memory yet
		} else {
			file_des->pipe->is_closed = true; // wait until pipe pair is closed to destroy the pair together
			return;
		}
	} else if (file_des->ftype == FILEDES_TYPE_SOCK) {
		// TODO:
		return;
	}
	if (file_des->flock_nodep) {
		file_rm_lock(file_des->node, file_des, &file_des->flock_nodep->fln_flock, 0, true);
		file_des->flock_nodep = NULL;
	}
	if (file_des->flags & O_TMPFILE) {
		file_unlink(file_des->pathname);
	}
	kfree(file_des->pathname);
	vfs_close(file_des->node); // check success?
	file_des->node = (void*)0xdeadbeef;
	KASSERT(!lock_do_i_hold(file_des->lk));
	lock_destroy(file_des->lk);
	filetable_nullout(p, file_des);
	file_des->refcount = 0;
	file_des->flags = 0;
	file_des->offset = 0;
	kfree(file_des);
}
static void init_pipe(struct pipe *p, bool is_writer, size_t buflen) {
	p->bufpos = 0;
	p->pair = NULL; // set elsewhere
	p->is_writer = is_writer;
	// pipes are destructed in pairs, so calling close() won't actually free the memory until its pair is closed
	p->is_closed = false;
	if (is_writer) {
		KASSERT(buflen <= PIPE_BUF_MAX);
		p->buflen = buflen;
		p->buf = kmalloc(buflen);
		KASSERT(p->buf);
		memset(p->buf, 0, buflen);
		p->wchan = NULL;
	} else {
		p->buf = NULL;
		p->buflen = 0;
		p->wchan = wchan_create("pipe wchan");
		spinlock_init(&p->wchan_lk);
	}
}
void pipe_destroy_reader(struct pipe *reader) {
	DEBUGASSERT(reader->is_closed && !reader->is_writer);
	if (reader->wchan) {
		wchan_destroy(reader->wchan);
		spinlock_cleanup(&reader->wchan_lk);
	}
	kfree(reader);
}
void pipe_destroy_writer(struct pipe *writer) {
	DEBUGASSERT(writer->is_closed && writer->is_writer);
	if (writer->buf) {
		kfree(writer->buf);
	}
	kfree(writer);
}
void pipe_destroy_pair(struct pipe *reader, struct pipe *writer) {
	DEBUG(DB_SYSCALL, "Destroying pipe pair\n");
	DEBUGASSERT(writer->pair == reader && reader->pair == writer);
	pipe_destroy_reader(reader);
	pipe_destroy_writer(writer);
}

struct filedes *pipe_create(struct proc *p, int flags, size_t buflen, int table_idx, int *err) {
	struct filedes *file_des = kmalloc(sizeof(*file_des));
	KASSERT(file_des);
	file_des->ftype = FILEDES_TYPE_PIPE;
	file_des->pathname = (buflen == 0) ? (char*)"pipe(reader)" :
																			 (char*)"pipe(writer)";
	file_des->node = NULL;
	file_des->pipe = kmalloc(sizeof(struct pipe));
	KASSERT(file_des->pipe);
	bool is_writer = (buflen > 0);
	init_pipe(file_des->pipe, is_writer, buflen);
	file_des->flags = flags;
	file_des->offset = 0; // not used for pipes
	file_des->refcount = 1;
	file_des->lk = NULL; // not used for pipes
	file_des->latest_fd = -1;
	int fd = filetable_put(p, file_des, table_idx);
	if (fd == -1) {
		*err = EMFILE;
		return NULL;
	}
	file_des->latest_fd = fd;
	file_des->pipe->fd = fd;
	return file_des;
}

void pipe_signal_can_read(struct pipe *reader) {
	spinlock_acquire(&reader->wchan_lk);
	wchan_wakeone(reader->wchan, &reader->wchan_lk);
	spinlock_release(&reader->wchan_lk);
}

int pipe_read_nonblock(struct pipe *reader, struct pipe *writer, userptr_t ubuf, size_t count, int *err) {
	// read should be ready now
	DEBUGASSERT(count <= writer->buflen);
	int copyout_res = copyout(writer->buf, ubuf, count);
	if (copyout_res != 0) {
		*err = copyout_res;
		return -1;
	}
	unsigned pos = writer->bufpos;
	if (pos > count) { // there's more left in the buffer
		// move the remaining buffer (tail) to the head and zero out the new tail
		memmove(writer->buf, writer->buf + count, writer->buflen - count);
		memset(writer->buf + (writer->buflen - count), 0, count);
		//DEBUG(DB_SYSCALL, "Pipe buffer left in writer: %s\n", writer->buf);
		writer->bufpos -= count;
		reader->buflen -= count;
	} else { // read everything in buffer, so zero it out
		memset(writer->buf, 0, writer->buflen);
		writer->bufpos = 0;
		reader->buflen = 0;
	}
	return count;
}

int pipe_read_block(struct pipe *reader, struct pipe *writer, userptr_t ubuf, size_t count, int *err) {
	reader->buflen = count;
	spinlock_acquire(&reader->wchan_lk);
	wchan_sleep(reader->wchan, &reader->wchan_lk);
	spinlock_release(&reader->wchan_lk);
	// check if we're being notified of write-end closure
	if (writer->is_closed) {
		char eof_buf[1];
		eof_buf[0] = '\0';
		copyout(eof_buf/*EOF*/, ubuf, 1);
		return 1;
	}
	return pipe_read_nonblock(reader, writer, ubuf, count, err);
}

// close file descriptor for current process by decreasing refcount on description,
// and freeing the resources if the refcount is 0.
// NOTE: the caller must call filetable_put(proc, NULL, fd) to actually have the
// filedes be unavailable to the process.
void filedes_close(struct proc *p, struct filedes *file_des) {
	KASSERT(file_des);
	if (!p) p = curproc;
	DEBUGASSERT(file_des->refcount > 0);
	if (file_des == devnull) {
		return;
	}
	if (file_des->refcount > 0) {
		file_des->refcount--;
	}
	if (file_des->refcount <= 0) {
		filedes_destroy(p, file_des);
	}
}

struct filedes *filedes_open(struct proc *p, char *pathname, struct vnode *node, int flags, int table_idx, int *errcode) {
	if (!p) p = curproc;
	return filedes_create(p, pathname, node, flags, table_idx, errcode);
}

static int filedes_inherit(struct proc *p, struct filedes *file_des, int idx, int *errcode) {
	DEBUGASSERT(idx >= 0);
	file_des->refcount++;
	int put_res = filetable_put(p, file_des, idx);
	if (put_res == -1) {
		*errcode = EMFILE;
		return -1;
	}
	return 0;
}

// NOTE: callers should report EMFILE (too many open files for process) if return value is negative
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
	if (!file_des->node) { return false; }
	return vnode_is_device(file_des->node);
}
bool filedes_is_seekable(struct filedes *file_des) {
	KASSERT(file_des);
	if (file_des->ftype != FILEDES_TYPE_REG) return false;
	if (!file_des->node) return false;
	return VOP_ISSEEKABLE(file_des->node);
}
bool filedes_is_console(struct filedes *file_des) {
	if (file_des == devnull) return false;
	return filedes_is_device(file_des);
}
bool filedes_is_lockable(struct filedes *file_des) {
	if (file_des == devnull || !file_des->node) {
		return false;
	}
	if (filedes_is_device(file_des)) {
		return false;
	}
	return file_des->ftype == FILEDES_TYPE_REG;
}

off_t filedes_size(struct filedes *file_des, int *errcode) {
	if (file_des->ftype != FILEDES_TYPE_REG || !file_des->node) {
		*errcode = EBADF;
		return -1;
	}
	struct stat st;
	int res = filedes_stat(file_des, &st, errcode);
	if (res != 0) {
		return res; // errcode set above
	}
	return st.st_size;
}

int filedes_stat(struct filedes *file_des, struct stat *st, int *errcode) {
	KASSERT(file_des);
	if (file_des->ftype != FILEDES_TYPE_REG || !file_des->node) {
		*errcode = EBADF;
		return -1;
	}
	int res = VOP_STAT(file_des->node, st); // fills out stat struct
	if (res != 0) {
		*errcode = res;
		return -1;
	}
	return 0;
}

int filedes_fcntl(struct filedes *file_des, int cmd, int flag, int *errcode) {
	switch (cmd) {
		case F_SETFD:
			switch (flag) {
				case O_CLOEXEC:
					file_des->flags |= O_CLOEXEC;
					KASSERT(file_des->flags & O_CLOEXEC);
					return 0;
				default:
					*errcode = EINVAL;
					return -1;
			}
			*errcode = EINVAL;
			return -1;
		case F_GETFD:
			(void)flag;
			return (file_des->flags & O_CLOEXEC); // O_CLOEXEC is the only FD flag
		case F_SETFL:
			switch(flag) {
				case O_NONBLOCK:
					file_des->flags |= O_NONBLOCK;
					return 0;
			default:
				*errcode = EINVAL;
				return -1;
			}
		case F_GETFL:
			(void)flag;
			return (file_des->flags & ~O_CLOEXEC); // ignore CLOEXEC, it's considered a FD flag
		case F_GETPATH: {
			(void)flag;
			userptr_t pathbuf = (userptr_t)flag;
			if (pathbuf == (userptr_t)0) {
				*errcode = EFAULT;
				return -1;
			}
			int copy_res = 0;
			if (file_des->pathname == NULL) {
				copy_res = copyoutstr("\0", pathbuf, 1, NULL);
			}
			copy_res = copyoutstr(file_des->pathname, pathbuf, PATH_MAX, NULL);
			if (copy_res == 0) {
				return 0;
			} else {
				*errcode = copy_res;
				return -1;
			}
		}
		default:
			*errcode = EINVAL;
			return -1;
	}
}

bool file_is_open(int fd) {
	return filetable_get(curproc, fd) != NULL;
}

bool file_is_readable(char *path) {
	(void)path;
	return true; // TODO: implement after support for access() syscall
}

bool file_is_writable(char *path) {
	(void)path;
	return true; // TODO: implement after support for access() syscall
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
		return ENOENT;
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

int file_access(char *path, int mode, int *errcode) {
	(void)mode; // TODO: check permissions for file
	struct vnode *fnode;
	int res = vfs_lookup(path, &fnode);
	if (res != 0) {
		*errcode = res;
		return -1;
	}
	VOP_DECREF(fnode);
	return 0;
}

bool file_is_dir(int fd) {
	struct filedes *file_des = filetable_get(curproc, fd);
	if (!file_des) return false;
	if (file_des->ftype != FILEDES_TYPE_REG || !file_des->node) {
		return false;
	}
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
	struct vnode *node = NULL;
	int result = vfs_open(path, openflags, mode, &node);
	if (result != 0) {
		if (node) {
			vfs_close(node);
		}
		*errcode = result;
		return -1;
	}
	struct filedes *new_filedes = filedes_open(curproc, path, node, openflags, -1, errcode);
	if (!new_filedes) {
		if (node) {
			vfs_close(node);
		}
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
	return new_filedes->latest_fd;
}

int file_read(struct filedes *file_des, struct uio *io, int *errcode) {
	if (!filedes_is_device(file_des) && !filedes_is_readable(file_des)) {
    *errcode = EBADF;
		return -1;
  }
	KASSERT(file_des->ftype == FILEDES_TYPE_REG);
	if (file_des == devnull) {
		return 0;
	}
	DEBUGASSERT(file_des->node);
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
	KASSERT(file_des->ftype == FILEDES_TYPE_REG);
	if (file_des == devnull) {
		return io->uio_iov->iov_len;
	}
	DEBUGASSERT(file_des->node);
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

int file_flock(struct filedes *file_des, int op) {
	KASSERT(file_des);
	if (!filedes_is_lockable(file_des)) {
		return EINVAL;
	}
	struct flock flock;
	bzero(&flock, sizeof(struct flock));
	flock.l_start = 0;
	flock.l_whence = SEEK_SET;
	if (op != LOCK_UN) {
		flock.l_type = (op & LOCK_SH) ? F_RDLCK : F_WRLCK;
	}
	flock.l_pid = curproc->pid;
	flock.l_len = 0; // represents whole file
	int lock_res = VOP_ADVLOCK(file_des->node, file_des, op, &flock);
	if (lock_res != 0) {
		DEBUGASSERT(lock_res > 0); // should return an errno-compatible error
		return lock_res; // error
	}
	if (op == LOCK_UN) {
		file_des->fi_flags &= ~FD_LOCK_EX;
		file_des->fi_flags &= ~FD_LOCK_SH;
	} else if (op & LOCK_EX) {
		file_des->fi_flags |=  FD_LOCK_EX;
		file_des->fi_flags &= ~FD_LOCK_SH;
	} else if (op & LOCK_SH) {
		file_des->fi_flags |=  FD_LOCK_SH;
		file_des->fi_flags &= ~FD_LOCK_EX;
	} else {
		KASSERT(0); // unreachable, VOP_ADVLOCK should error out if op is invalid
	}
	return 0;
}

static struct flock_node *flocklist_head() {
	return flocklist_virt_head;
}

static struct flock_node *flocklist_tail() {
	return flocklist_virt_tail;
}

static struct flock_node *first_flock_for_file(struct vnode *vnode) {
	KASSERT(vnode);
	struct flock_node *fln = flocklist_head()->fln_next;
	while (fln && fln != flocklist_virt_tail) {
		if (fln->fln_vnode == vnode) return fln;
		fln = fln->fln_next;
	}
	return NULL;
}

static struct flock_node *first_flock_for_file_not_held_by_me(struct vnode *vnode) {
	KASSERT(vnode);
	struct flock_node *fln = flocklist_head()->fln_next;
	while (fln && fln != flocklist_virt_tail) {
		if (fln->fln_vnode == vnode && fln->fln_flock.l_pid != curproc->pid) return fln;
		fln = fln->fln_next;
	}
	return NULL;
}

static bool is_only_flock_for_file(struct flock_node *fln, struct vnode *vnode) {
	struct flock_node *fln_cur = flocklist_head()->fln_next;
	DEBUGASSERT(fln_cur != NULL);
	while (fln_cur && fln_cur != flocklist_virt_tail) {
		if (fln_cur != fln && fln_cur->fln_vnode == vnode) return false;
		fln_cur = fln_cur->fln_next;
	}
	return true;
}

static int add_new_flock(struct vnode *vnode, struct flock *flock, struct filedes *fdes) {
	struct flock_node *fln = kmalloc(sizeof(*fln));
	KASSERT(fln);
	bzero(fln, sizeof(*fln));
	fln->fln_flock = *flock;
	fln->fln_vnode = vnode;
	fln->fln_fdp = fdes;
	spinlock_init(&fln->fln_wchan_lk);
	fln->fln_wchan = wchan_create("flock");
	fln->fln_next = NULL; fln->fln_prev = NULL;
	VOP_INCREF(vnode);
	struct flock_node *tail_virt, *head_virt, *tail_real;
	tail_virt = flocklist_tail();
	head_virt = flocklist_head();
	tail_real = tail_virt->fln_prev == head_virt ? NULL : tail_virt->fln_prev;

	if (!tail_real) { // first real entry
		fln->fln_prev = head_virt;
		fln->fln_next = tail_virt;
		tail_virt->fln_prev = fln;
		head_virt->fln_next = fln;
	} else {
		KASSERT(tail_real->fln_next == tail_virt);
		tail_real->fln_next = fln;
		fln->fln_prev = tail_real;
		fln->fln_next = tail_virt;
		tail_virt->fln_prev = fln;
	}
	fdes->flock_nodep = fln;
	return 0;
}

/*
	Flock semantics:

	Taking exclusive lock for file thru fd X:
	  1) No locks exists for file, create it
	  2) Shared lock exists for file:
	       * if it's for same filedes and there aren't any other (shared) locks for the file, upgrade it to exclusive
	       * otherwise, block until all shared locks are unlocked and try again
	  3) Exclusive lock exists for file:
	       * if it's for same filedes, return an error if it's held by current process. If not, block until it's removed and try again.
	       * if it's for different filedes, block until exclusive lock is unlocked and try again

	Taking shared lock for file thru fd X:
	  1) No locks exist for file, create it
	  2) Shared lock exists for file, create it
	  3) Exclusive lock exists for file:
	      * if it's for same filedes, downgrade it to shared and notify waiters
	      * if it's for diff filedes, block then retry ONLY if current process doesn't hold it
	      * otherwise, return error

	Unlocking a lock for file thru fd X:
	  1) If last shared lock, notify waiters
	  2) If exclusive lock, notify waiters
*/
int file_try_lock(struct vnode *vnode, struct filedes *fd_p, struct flock *flock, int flags) {
	struct flock_node *fln, *fln_samefile;
	bool taking_exlock = flock->l_type == F_WRLCK;
	bool taking_shlock = flock->l_type == F_RDLCK;

retry_lock: {

	fln = flocklist_head()->fln_next;

	if (fln == NULL) { // first active lock ever
		DEBUG(DB_SYSCALL, "Creating first active flock.\n");
		return add_new_flock(vnode, flock, fd_p);
	}

	fln_samefile = first_flock_for_file(vnode);
	if (!fln_samefile) {
		DEBUG(DB_SYSCALL, "Creating first flock for file.\n");
		return add_new_flock(vnode, flock, fd_p);
	}
	bool lock_is_shlock = fln_samefile->fln_flock.l_type == F_RDLCK;
	bool lock_is_exlock = !lock_is_shlock;
	bool lock_is_for_same_fdes = fln_samefile->fln_fdp == fd_p;
	bool lock_held_by_me = fln_samefile->fln_flock.l_pid == curproc->pid;

	if (taking_exlock) {
		if (lock_is_shlock) {
			if (lock_is_for_same_fdes && is_only_flock_for_file(fln_samefile, vnode)) {
				// safe to upgrade to exclusive, curproc takes ownership
				DEBUG(DB_SYSCALL, "Upgrading SH lock to EX lock.\n");
				fln_samefile->fln_flock = *flock;
				return 0;
			} else {
				struct flock_node *fln_samefile_held_by_other_proc = first_flock_for_file_not_held_by_me(vnode);
				if (fln_samefile_held_by_other_proc) {
					DEBUG(DB_SYSCALL, "Tried to take EX lock, blocking due SH lock(s)\n");
					// block until all shared locks are gone for file (actually just blocks until this one is released, then tries again)
					spinlock_acquire(&fln_samefile_held_by_other_proc->fln_wchan_lk);
					wchan_sleep_no_reacquire_on_wake(fln_samefile_held_by_other_proc->fln_wchan, &fln_samefile_held_by_other_proc->fln_wchan_lk);
					DEBUG(DB_SYSCALL, "Retrying EX lock\n");
					goto retry_lock;
				} else { // we hold the shared lock that's causing the exclusive one to fail (thru another filedes)
					// don't want to deadlock here, so we just return
					if (flags & LOCK_NB) {
						return EAGAIN;
					} else {
						return EINVAL;
					}
				}
			}
		} else if (lock_is_exlock) {
			if (lock_held_by_me) {
				return EINVAL;
			} else {
				if (flags & LOCK_NB) {
					return EAGAIN;
				}
				spinlock_acquire(&fln_samefile->fln_wchan_lk);
				wchan_sleep_no_reacquire_on_wake(fln_samefile->fln_wchan, &fln_samefile->fln_wchan_lk);
				goto retry_lock;
			}
		}
		panic("BUG: unreachable");
	} else { // taking shared lock
		DEBUGASSERT(taking_shlock);
		if (lock_is_shlock) {
			if (lock_is_for_same_fdes && !lock_held_by_me) {
				fln_samefile->fln_flock = *flock; // gain ownership
				return 0;
			} else if (lock_is_for_same_fdes && lock_held_by_me) {
				return EINVAL;
			} else {
				DEBUGASSERT(!lock_is_for_same_fdes);
				DEBUG(DB_SYSCALL, "Creating another shared lock for file thru new fdes.\n");
				return add_new_flock(vnode, flock, fd_p);
			}
		} else { // existing lock is exclusive lock for file and we're trying to take a shared lock
			DEBUGASSERT(lock_is_exlock);
			if (lock_is_for_same_fdes) {
				DEBUG(DB_SYSCALL, "Downgrading EX lock to SH lock thru same fdes.\n");
				fln_samefile->fln_flock = *flock; // downgrade to shared and potentially gain ownership if we aren't the current holder
				spinlock_acquire(&fln_samefile->fln_wchan_lk);
				wchan_wakeall(fln_samefile->fln_wchan, &fln_samefile->fln_wchan_lk);
				spinlock_release(&fln_samefile->fln_wchan_lk);
				return 0;
			} else {
				if (lock_held_by_me) {
					return EINVAL;
				}
				if (flags & LOCK_NB) {
					return EAGAIN;
				}
				// block until exclusive lock is removed or downgraded
				spinlock_acquire(&fln_samefile->fln_wchan_lk);
				wchan_sleep_no_reacquire_on_wake(fln_samefile->fln_wchan, &fln_samefile->fln_wchan_lk);
				goto retry_lock;
			}
		}
	}
} // retry_lock
	panic("BUG: unreachable");
}

int file_rm_lock(struct vnode *vnode, struct filedes *fd_p, struct flock *flock, int flags, bool forceremove) {
	(void)flags; (void)flock;
	struct flock_node *fln, *head;
	struct flock_node *fln_found = NULL;
	head = flocklist_head()->fln_next;
	fln = head;
	while (fln && fln != flocklist_virt_tail) {
		if (fln->fln_vnode == vnode && (fln->fln_flock.l_pid == curproc->pid || forceremove) &&
			  fln->fln_fdp == fd_p) {
			fln_found = fln;
			break;
		}
		fln = fln->fln_next;
	}
	if (!fln_found) {
		return EINVAL;
	}

	// next and prev should always exist due to virtual head and tail in list
	fln_found->fln_prev->fln_next = fln_found->fln_next;
	fln_found->fln_next->fln_prev = fln_found->fln_prev;

	struct wchan *wchan = fln_found->fln_wchan;
	struct spinlock *wchan_lk = &fln_found->fln_wchan_lk;

	spinlock_acquire(wchan_lk);
	wchan_wakeall(wchan, wchan_lk);
	spinlock_release(wchan_lk);

	VOP_DECREF(fln_found->fln_vnode);
	DEBUG(DB_SYSCALL, "Removed flock entry (on %s)\n", forceremove ? "close" : "unlock");

	wchan_destroy(wchan);
	spinlock_cleanup(wchan_lk);
	kfree(fln_found);
	fd_p->flock_nodep = NULL;
	return 0;
}

int file_create_pipe_pair(int *reader_fd, int *writer_fd, size_t buflen) {
	if (buflen == 0) {
		return EINVAL;
	} else if (buflen > PIPE_BUF_MAX) {
		return ENOMEM;
	}
	int errcode = 0;
	struct filedes *reader = pipe_create(curproc, O_RDONLY, 0, -1, &errcode);
	if (!reader) {
		return errcode;
	}
	struct filedes *writer = pipe_create(curproc, O_WRONLY, buflen, -1, &errcode);
	if (!writer) {
		reader->pipe->is_closed = true;
		filedes_close(curproc, reader);
		return errcode;
	}
	reader->pipe->pair = writer->pipe;
	writer->pipe->pair = reader->pipe;
	*reader_fd = reader->latest_fd;
	*writer_fd = writer->latest_fd;
	return 0;
}

/* NOTE: doesn't include terminating NULL at end of array */
int proc_environ_numvars(struct proc *p) {
	DEBUGASSERT(p && p->p_environ);
	int i = 0;
	while (p->p_environ[i] != NULL) {
		i++;
	}
	return i;
}

static void proc_free_environ(char **environ) {
	DEBUGASSERT(environ);
	int i = 0;
	while (environ[i] != NULL) {
		kfree(environ[i]);
		i++;
	}
	kfree(environ);
}

static char **proc_dup_environ(const char **environ_proto, size_t num_vars/* includes NULL */) {
	DEBUGASSERT(environ_proto);
	if (num_vars == 0) {
		while (environ_proto[num_vars] != NULL) { num_vars++; }
		num_vars++; // 1 more for NULL
	}
	char **env = kmalloc(num_vars);
	KASSERT(env);
	int i = 0;
	while (environ_proto[i] != NULL) {
		env[i] = kstrdup(environ_proto[i]);
		KASSERT(env[i]);
		i++;
	}
	env[i] = NULL;
	return env;
}

static char** proc_default_environ() {
	return proc_dup_environ(default_proc_environ, sizeof(default_proc_environ));
}

/*
 * Create a proc structure with empty address space and file table.
 */
struct proc *proc_create(const char *name) {
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

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;
	/* userspace environment variables array */
	proc->p_environ = proc_default_environ();
	proc->p_uenviron = (userptr_t)0;

	proc->pid = INVALID_PID;
	proc->file_table = kmalloc(FILE_TABLE_LIMIT * sizeof(struct filedes*));
	if (proc->file_table == NULL) {
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}
	bzero((void *)proc->file_table, FILE_TABLE_LIMIT * sizeof(struct filedes*));
	proc->file_table_refcount = 1;
	spinlock_init(&proc->p_lock);
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
	KASSERT(proc != kswapproc);

	DEBUG(DB_VM, "Destroying process %s\n", proc->p_name);

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

	proc->file_table_refcount--;
	if (proc->file_table_refcount == 0) {
		for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
			struct filedes *file_des;
			if ((file_des = filetable_get(proc, i)) != NULL) {
				filedes_close(proc, file_des);
			}
		}
		bzero((void *)proc->file_table, FILE_TABLE_LIMIT * sizeof(struct filedes*));
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
			DEBUG(DB_VM, "Calling as_deactivate in proc_destroy for %s\n", proc->p_name);
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		DEBUG(DB_VM, "Calling as_destroy in proc_destroy for %s\n", proc->p_name);
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
	proc_free_environ(proc->p_environ);
	proc->p_environ = NULL;
	kfree(proc->p_name);
	lock_destroy(proc->p_mutex);
	kfree(proc);
}

int proc_fork(struct proc *parent_pr, struct thread *parent_th, struct trapframe *tf, int *err) {
	KASSERT(is_current_userspace_proc(parent_pr));
	struct proc *child_pr = NULL;
	child_pr = proc_create(parent_pr->p_name);
	if (!child_pr) {
		*err = ENOMEM; // just a guess
		return -1;
	}
	child_pr->p_parent = parent_pr;
	child_pr->p_addrspace = NULL;
	lock_acquire(parent_pr->p_mutex);
	int res = as_copy(parent_pr->p_addrspace, &child_pr->p_addrspace);
	if (res != 0) {
		lock_release(parent_pr->p_mutex);
		proc_destroy(child_pr);
		*err = res;
		return -1;
	}
	KASSERT(child_pr->p_addrspace != NULL);
	res = proc_inherit_filetable(parent_pr, child_pr);
	if (res != 0) {
		lock_release(parent_pr->p_mutex);
		proc_destroy(child_pr);
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

int proc_close_cloexec_files(struct proc *p) {
	int num_closed = 0;
	for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
		struct filedes *des = filetable_get(p, i);
		if (!des) {
			continue;
		}
		if (des->flags & O_CLOEXEC) {
			filedes_close(p, des);
			filetable_put(p, NULL, i);
			num_closed++;
		}
	}
	return num_closed;
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
	devnull = kmalloc(sizeof(struct filedes));
	KASSERT(devnull);
	devnull->pathname = kstrdup("/dev/null");
	devnull->ftype = FILEDES_TYPE_REG;
	devnull->node = NULL;
	devnull->pipe = NULL;
	devnull->sock = NULL;
	devnull->flags = O_RDWR;
	devnull->offset = 0;
	devnull->refcount = 1;
	devnull->latest_fd = -1;
	struct flock_node *virt_head = kmalloc(sizeof(*virt_head));
	struct flock_node *virt_tail = kmalloc(sizeof(*virt_tail));
	KASSERT(virt_head); KASSERT(virt_tail);
	bzero(virt_head, sizeof(*virt_head));
	bzero(virt_tail, sizeof(*virt_tail));
	virt_head->fln_next = virt_tail;
	virt_head->fln_prev = NULL;
	virt_tail->fln_next = NULL;
	virt_tail->fln_prev = virt_head;
	flocklist_virt_head = virt_head;
	flocklist_virt_tail = virt_tail;
	file_locks = virt_head;
	KASSERT(file_locks->fln_next->fln_prev == virt_head);
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
	int errcode = 0;
	filedes_open(
		p,
		(char*)special_filedes_name(0),
		console_in,
		special_filedes_flags(0),
		0,
		&errcode
	);
	if (errcode != 0) {
		return errcode;
	}
	filedes_open(
		p,
		(char*)special_filedes_name(1),
		console_out,
		special_filedes_flags(1),
		1,
		&errcode
	);
	if (errcode != 0) {
		return errcode;
	}
	filedes_open(
		p,
		(char*)special_filedes_name(2),
		console_err,
		special_filedes_flags(2),
		2,
		&errcode
	);
	if (errcode != 0) {
		return errcode;
	}
	return 0;
}

int proc_redir_standard_streams(struct proc *p, int newfd) {
	struct filedes *new_filedes;
	if (newfd == FD_DEV_NULL) {
		new_filedes = devnull;
	} else {
		new_filedes = filetable_get(p, newfd);
		if (!new_filedes) {
			return -1;
		}
	}
	int put_res = 0;
	filedes_close(p, filetable_get(p, 0));
	filedes_close(p, filetable_get(p, 1));
	filedes_close(p, filetable_get(p, 2));
	put_res = filetable_put(p, new_filedes, 0);
	if (put_res < 0) return -1;
	put_res = filetable_put(p, new_filedes, 1);
	if (put_res < 0) return -1;
	put_res = filetable_put(p, new_filedes, 2);
	if (put_res < 0) return -1;
	return 0;
}

int proc_inherit_filetable(struct proc *parent, struct proc *child) {
	struct filedes *fd = NULL;
	int put_res = 0;
	int errcode = 0;
	for (int i = 0; i < FILE_TABLE_LIMIT; i++) {
		fd = filetable_get(parent, i);
		if (fd) {
			put_res = filedes_inherit(child, fd, i, &errcode);
			if (put_res == -1) {
				return errcode;
			}
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
	newproc->p_stacktop = USERSTACK;
	newproc->p_stacksize = VM_STACKPAGES * PAGE_SIZE;

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

// Clones a process, which means creating a new process using the same address space as
// the passed in process, and a separate stack inside this address space.
// See clone(2) (and the CLONE_VM flag) for details.
struct proc *proc_clone(struct proc *old, vaddr_t new_stacktop, size_t new_stacksize, int flags, int *err) {
	DEBUGASSERT(old->p_addrspace != NULL);
	KASSERT(is_current_userspace_proc(old));
	(void)flags;
	vaddr_t old_stacktop = old->p_stacktop;
	vaddr_t old_stackbottom = old->p_stacktop - old->p_stacksize;
	vaddr_t new_stackbottom = new_stacktop - new_stacksize;
	if (vm_regions_overlap(old_stackbottom, old_stacktop, new_stackbottom, new_stacktop)) {
		DEBUG(DB_SYSCALL, "proc_clone failed, stacks overlap!\n");
		*err = EINVAL;
		return NULL;
	}
	if (!as_heap_region_exists(old->p_addrspace, new_stackbottom, new_stacktop)) {
		DEBUG(DB_SYSCALL, "proc_clone failed, new stack space is invalid heap for addrspace!\n");
		*err = EINVAL;
		return NULL;
	}
	size_t name_size = strlen(old->p_name)+1+8;
	char *clone_name = kmalloc(name_size); // ' (clone)'
	snprintf(clone_name, name_size, "%s (clone)", old->p_name);
	struct proc *clone = proc_create(clone_name);
	if (!clone) {
		DEBUG(DB_SYSCALL, "proc_clone failed: proc_create failure\n");
		kfree(clone_name);
		*err = ENOMEM;
		return NULL;
	}
	kfree(clone_name); // proc_create() kstrdup's the name, so we can free it here
	clone->p_addrspace = old->p_addrspace; // share address space with old process
	clone->p_addrspace->refcount++;
	clone->file_table_refcount++;
	old->file_table_refcount++;
	kfree(clone->file_table); // share file_table with old process
	clone->file_table = old->file_table;
	clone->p_parent = old->p_parent;
	clone->p_cwd = old->p_cwd;
	clone->pid = INVALID_PID; // given in thread_fork
	clone->p_stacktop = new_stacktop;
	clone->p_stacksize = new_stacksize;
	return clone;
}

// Is this a process created by the clone() syscall? If so, it shares an address space
// with the calling process, and its userspace stack is located inside this address space's
// heap.
bool proc_is_clone(struct proc *p) {
	if (!p->p_addrspace || p->p_stacktop == 0 || p->p_stacksize == 0) {
		return false;
	}
	vaddr_t stacktop = p->p_stacktop;
	vaddr_t stackbtm = p->p_stacktop - p->p_stacksize;
	return as_heap_region_exists(p->p_addrspace, stackbtm, stacktop);
}

int proc_send_signal(struct proc *p, int sig, int *errcode) {
	struct thread *t = thread_find_by_id(p->pid);
	if (!t) {
		*errcode = ESRCH;
		return -1;
	}
	if (t->t_state == S_ZOMBIE) {
		*errcode = ESRCH;
		return -1;
	}
	switch (sig) {
	case SIGSTOP:
	case SIGCONT:
	case SIGKILL:
		return thread_send_signal(t, sig);
	// not yet implemented
	default:
		*errcode = EINVAL;
		return -1;
	}
}

// list user processes in the system
void proc_list(void) {
	struct proc *p = NULL;
	struct thread *t = NULL;
	for (int i = 0; i < MAX_USERPROCS; i++) {
		p = userprocs[i];
		if (p) {
			t = thread_find_by_id(p->pid);
			KASSERT(t);
			if (t->t_state == S_ZOMBIE) { continue; }
			const char *stopped = "";
			if (t->t_is_stopped) {
				stopped = " (STOPPED)";
			}
			kprintf("%s (%d)\t[%s%s]\n", p->p_name, (int)p->pid, threadstate_name(t->t_state), stopped);
		}
	}
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
 * one for later restoration or disposal. NOTE: newas can be NULL.
 */
struct addrspace *proc_setas(struct addrspace *newas) {
	struct addrspace *oldas;
	struct proc *p = curproc;

	KASSERT(p != NULL);
	KASSERT(p != kproc);

	spinlock_acquire(&p->p_lock);
	oldas = p->p_addrspace;
	p->p_addrspace = newas;
	if (newas)
		newas->pid = p->pid;
	spinlock_release(&p->p_lock);
	return oldas;
}

void proc_define_stack(struct proc *p, vaddr_t stacktop, size_t stacksize) {
	KASSERT(stacksize % PAGE_SIZE == 0);
	KASSERT(stacksize >= PAGE_SIZE);
	p->p_stacktop = stacktop;
	p->p_stacksize = stacksize;
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

int file_select(unsigned nfds, struct fd_set *reads, struct fd_set *writes,
								struct fd_set *exceptions, struct timeval *timeout,
								int *errcode, int *num_ready) {
	(void)nfds;
	int total_ready = 0;

	struct fd_set reads_cpy = *reads;
	struct fd_set writes_cpy = *writes;
	struct fd_set exceptions_cpy = *exceptions;
	FD_ZERO(reads);
	FD_ZERO(writes);
	FD_ZERO(exceptions);

	for (unsigned i = 0; i < reads_cpy.fds; i++) {
		int fd = reads_cpy.fd_list[i];
		DEBUGASSERT(fd > 0);
		if (io_check_ready(fd-1, IO_TYPE_READ)) {
			FD_SETREADY(fd-1, reads);
			total_ready += 1;
		}
	}
	for (unsigned i = 0; i < writes_cpy.fds; i++) {
		int fd = writes_cpy.fd_list[i];
		DEBUGASSERT(fd > 0);
		if (io_check_ready(fd-1, IO_TYPE_WRITE)) {
			FD_SETREADY(fd-1, writes);
			total_ready += 1;
		}
	}
	for (unsigned i = 0; i < exceptions_cpy.fds; i++) {
		int fd = exceptions_cpy.fd_list[i];
		DEBUGASSERT(fd > 0);
		if (io_check_ready(fd-1, IO_TYPE_EXCEPTION)) {
			FD_SETREADY(fd-1, exceptions);
			total_ready += 1;
		}
	}
	if (total_ready > 0) {
		DEBUG(DB_SYSCALL, "file_select returning immediately without blocking (ready IO)\n");
		*num_ready = total_ready;
		return 0; // no need to poll or sleep, some conditions are ready
	}

	struct timeval now;
	timeval_now(&now);
	if (timeout) {
		int timeval_cmp_res = timeval_cmp(&now, timeout);
		if (timeval_cmp_res == 0 || timeval_cmp_res == 1) { // timeout is in the past, just return
			DEBUG(DB_SYSCALL, "Timeout value is in the past, returning from file_select without blocking\n");
			*errcode = EINVAL;
			return -1;
		}
	}

	// setup polling if timeout is in the future, or no timeout given
	struct io_poll poll;
	io_poll_init(&poll);
	for (unsigned i = 0; i < reads_cpy.fds; i++) {
		int fd = reads_cpy.fd_list[i];
		DEBUGASSERT(fd > 0);
		io_poll_setup(&poll, fd-1, IO_TYPE_READ, reads, sel_wchan, &sel_wchan_lk);
	}
	for (unsigned i = 0; i < writes_cpy.fds; i++) {
		int fd = writes_cpy.fd_list[i];
		DEBUGASSERT(fd > 0);
		io_poll_setup(&poll, fd-1, IO_TYPE_WRITE, writes, sel_wchan, &sel_wchan_lk);
	}
	for (unsigned i = 0; i < exceptions_cpy.fds; i++) {
		int fd = exceptions_cpy.fd_list[i];
		DEBUGASSERT(fd > 0);
		io_poll_setup(&poll, fd-1, IO_TYPE_EXCEPTION, exceptions, sel_wchan, &sel_wchan_lk);
	}

	int poll_start_res = io_poll_start(&poll);
	if (poll_start_res != 0) {
		*errcode = poll_start_res;
		return -1;
	}

	DEBUG(DB_SYSCALL, "file_select putting current thread to sleep\n");
	int timed_out = 0;

	spinlock_acquire(&sel_wchan_lk);
	if (timeout) {
		timed_out = wchan_sleep_timeout(sel_wchan, &sel_wchan_lk, timeout);
	} else {
		wchan_sleep(sel_wchan, &sel_wchan_lk);
	}
	spinlock_release(&sel_wchan_lk);

	io_poll_stop(&poll);
	DEBUG(DB_SYSCALL, "file_select thread woke up (%s)\n", timed_out ? "timed out" : "IO ready");
	if (timed_out == 1) {
		*num_ready = 0;
		return 0;
	} else {
		*num_ready = FDS_READY(reads) + FDS_READY(writes) + FDS_READY(exceptions);
		return 0;
	}
}

static void io_ready_broadcast(int fd, enum io_ready_type io_type, size_t min_bytes_ready) {
	(void)min_bytes_ready;
	struct io_pollset *pollset = io_listeners[fd];
	DEBUGASSERT(pollset != NULL);
	struct io_pollset_interest *interest = pollset->polls;
	for (unsigned i = 0; i < pollset->num_polls; i++) {
		if (interest->ready_type != io_type) {
			interest = interest->next;
			continue;
		}
		FD_SETREADY(fd, interest->fd_setp);
		spinlock_acquire(interest->wchan_lk);
		wchan_wake_specific(interest->t_wakeup, interest->wchan, interest->wchan_lk);
		spinlock_release(interest->wchan_lk);
		interest = interest->next;
	}
}

void io_is_ready(pid_t pid, int fd, enum io_ready_type io_type, size_t min_bytes_ready) {
	(void)pid;
	if (io_listeners[fd] && (io_listeners[fd]->io_poll_types & io_type) != 0) {
		io_ready_broadcast(fd, io_type, min_bytes_ready);
	}
}

bool io_check_ready(int fd, enum io_ready_type type) {
	(void)fd;
	(void)type;
	return false;
}

void io_poll_init(struct io_poll *poll) {
	poll->running = false;
	poll->num_interests = 0;
	poll->interests = NULL;
}

void io_poll_setup(struct io_poll *poll, int fd, enum io_ready_type type, struct fd_set *fd_setp,
									 struct wchan *wchan, struct spinlock *wchan_lk) {
	DEBUGASSERT(!poll->running);
	struct io_pollset_interest *interest = kmalloc(sizeof(*interest));
	KASSERT(interest);
	interest->fd = fd;
	interest->fd_setp = fd_setp;
	interest->ready_type = type;
	interest->wchan = wchan;
	interest->wchan_lk = wchan_lk;
	interest->t_wakeup = curthread;
	interest->next = NULL;
	interest->poll = poll;
	// link it into poll->interests
	if (poll->interests) {
		struct io_pollset_interest *last = poll->interests;
		while (last->next) { last = last->next; }
		last->next = interest;
	} else {
		poll->interests = interest;
	}
	poll->num_interests++;
}

int io_poll_start(struct io_poll *poll) {
	if (poll->num_interests == 0 || poll->running) {
		return EINVAL;
	}
	struct io_pollset_interest *interest = poll->interests;
	int fd = interest->fd;
	struct io_pollset *pollset = io_listeners[fd];
	if (pollset == NULL) {
		pollset = kmalloc(sizeof(*pollset));
		KASSERT(pollset);
		memset(pollset, 0, sizeof(*pollset));
		io_listeners[fd] = pollset;
	}
	// NOTE: linking in the first interest links in all of them, because they're a linked list
	if (pollset->polls) {
		struct io_pollset_interest *last = pollset->polls;
		while (last->next) { last = last->next; }
		last->next = interest;
	} else {
		pollset->polls = interest;
	}

	pollset->num_polls += poll->num_interests;

	struct io_pollset_interest *cur = poll->interests;
	while (cur) {
		if ((pollset->io_poll_types & cur->ready_type) == 0) {
			pollset->io_poll_types |= cur->ready_type;
		}
		cur = cur->next;
	}
	poll->running = true;
	return 0;
}

// NOTE: assumes only 1 poll running at once! FIXME:
void io_poll_stop(struct io_poll *poll) {
	DEBUGASSERT(poll->running);
	struct io_pollset_interest *interest = poll->interests;
	struct io_pollset_interest *next;
	for (unsigned i = 0; i < poll->num_interests; i++) {
		DEBUGASSERT(interest && interest->poll == poll);
		int fd = interest->fd;
		if (io_listeners[fd]) {
			kfree(io_listeners[fd]);
			io_listeners[fd] = NULL;
		}
		next = interest->next;
		kfree(interest);
		interest = next;
	}
	poll->num_interests = 0;
	poll->interests = NULL;
	poll->running = false;
}
