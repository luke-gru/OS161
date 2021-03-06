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
#include <kern/errno.h>
#include <kern/syscall.h>
#include <lib.h>
#include <mips/trapframe.h>
#include <mips/specialreg.h>
#include <thread.h>
#include <current.h>
#include <syscall.h>
#include <proc.h>
#include <cpu.h>
#include <addrspace.h>
#include <clock.h>

/*
 * System call dispatcher.
 *
 * A pointer to the trapframe created during exception entry (in
 * exception-*.S) is passed in.
 *
 * The calling conventions for syscalls are as follows: Like ordinary
 * function calls, the first 4 32-bit arguments are passed in the 4
 * argument registers a0-a3. 64-bit arguments are passed in *aligned*
 * pairs of registers, that is, either a0/a1 or a2/a3. This means that
 * if the first argument is 32-bit and the second is 64-bit, a1 is
 * unused.
 *
 * This much is the same as the calling conventions for ordinary
 * function calls. In addition, the system call number is passed in
 * the v0 register.
 *
 * On successful return, the return value is passed back in the v0
 * register, or v0 and v1 if 64-bit. This is also like an ordinary
 * function call, and additionally the a3 register is also set to 0 to
 * indicate success.
 *
 * On an error return, the error code is passed back in the v0
 * register, and the a3 register is set to 1 to indicate failure.
 * (Userlevel code takes care of storing the error code in errno and
 * returning the value -1 from the actual userlevel syscall function.
 * See src/user/lib/libc/arch/mips/syscalls-mips.S and related files.)
 *
 * Upon syscall return the program counter stored in the trapframe
 * must be incremented by one instruction; otherwise the exception
 * return code will restart the "syscall" instruction and the system
 * call will repeat forever.
 *
 * If you run out of registers (which happens quickly with 64-bit
 * values) further arguments must be fetched from the user-level
 * stack, starting at sp+16 to skip over the slots for the
 * registerized values, with copyin().
 */
void
syscall(struct trapframe *tf)
{
	int callno;
	int32_t retval;
	int err = 0;

	KASSERT(curthread != NULL);
	KASSERT(curthread->t_curspl == 0);
	KASSERT(curthread->t_iplhigh_count == 0);

	callno = tf->tf_v0;

	/*
	 * Initialize retval to 0. Many of the system calls don't
	 * really return a value, just 0 for success and -1 on
	 * error. Since retval is the value returned on success,
	 * initialize it to 0 by default; thus it's not necessary to
	 * deal with it except for calls that return other values,
	 * like write.
	 */

	retval = 0;

	switch (callno) {
		case SYS_reboot:
			err = sys_reboot(tf->tf_a0);
			break;
	  case SYS___time:
			err = sys___time((userptr_t)tf->tf_a0, (userptr_t)tf->tf_a1);
			break;
		case SYS_open:
			err = sys_open((userptr_t)tf->tf_a0, (int)tf->tf_a1, (mode_t)tf->tf_a2, &retval);
			break;
		case SYS_write:
			err = sys_write((int)tf->tf_a0, (userptr_t)tf->tf_a1, (size_t)tf->tf_a2, &retval);
			break;
		case SYS_read:
			err = sys_read((int)tf->tf_a0, (userptr_t)tf->tf_a1, (size_t)tf->tf_a2, &retval);
			break;
		case SYS_access:
			err = sys_access((userptr_t)tf->tf_a0, (int)tf->tf_a1, &retval);
			break;
		case SYS_close:
			err = sys_close((int)tf->tf_a0, &retval);
			break;
		case SYS_fstat:
			err = sys_fstat((int)tf->tf_a0, (userptr_t)tf->tf_a1, &retval);
			break;
		case SYS_fcntl:
			err = sys_fcntl((int)tf->tf_a0, (int)tf->tf_a1, (int)tf->tf_a2, &retval);
			break;
		case SYS_lseek:
			err = sys_lseek((int)tf->tf_a0, (int32_t)tf->tf_a1, (int)tf->tf_a2, &retval);
			break;
		case SYS_remove: /* called unlink in other unix-likes */
			err = sys_remove((userptr_t)tf->tf_a0, &retval);
			break;
		case SYS_dup:
			err = sys_dup((int)tf->tf_a0, &retval);
			break;
		case SYS_dup2:
			err = sys_dup2((int)tf->tf_a0, (int)tf->tf_a1, &retval);
			break;
		case SYS_flock:
			err = sys_flock((int)tf->tf_a0, (int)tf->tf_a1, &retval);
			break;
		case SYS_mkdir:
			err = sys_mkdir((userptr_t)tf->tf_a0, (mode_t)tf->tf_a1, &retval);
			break;
		case SYS_getdirentry:
			err = sys_getdirentry((int)tf->tf_a0, (userptr_t)tf->tf_a1, (size_t)tf->tf_a2, &retval);
			break;
		case SYS_rmdir:
			err = sys_rmdir((userptr_t)tf->tf_a0, &retval);
			break;
		case SYS_getpid:
			err = sys_getpid(&retval);
			break;
		case SYS_chdir:
			err = sys_chdir((userptr_t)tf->tf_a0, &retval);
			break;
		case SYS___getcwd:
			err = sys_getcwd((userptr_t)tf->tf_a0, (size_t)tf->tf_a1, &retval);
			break;
		case SYS_fork:
			err = sys_fork(tf, &retval);
			break;
		case SYS_vfork:
			err = sys_vfork(tf, &retval);
			break;
		case SYS_execv:
			err = sys_execve((userptr_t)tf->tf_a0, (userptr_t)tf->tf_a1, (userptr_t)0, &retval);
			if (err == 0) {
				panic("should have started new process!");
			}
			break;
		case SYS_waitpid:
			err = sys_waitpid((pid_t)tf->tf_a0, (userptr_t)tf->tf_a1, (int)tf->tf_a2, &retval);
			break;
		case SYS_clone:
			err = sys_clone((userptr_t)tf->tf_a0, (uint32_t)tf->tf_a1, (size_t)tf->tf_a2, (int)tf->tf_a3, &retval);
			break;
		case SYS_sbrk:
			err = sys_sbrk((size_t)tf->tf_a0, &retval);
			break;
		case SYS_mmap:
			err = sys_mmap((size_t)tf->tf_a0, (int)tf->tf_a1, (int)tf->tf_a2, (int)tf->tf_a3, (off_t)tf->tf_v1, &retval);
			break;
		case SYS_msync:
			err = sys_msync((uint32_t)tf->tf_a0, (size_t)tf->tf_a1, (int)tf->tf_a2, &retval);
			break;
		case SYS_munmap:
			err = sys_munmap((uint32_t)tf->tf_a0, &retval);
			break;
		case SYS_signal:
			err = sys_signal((int)tf->tf_a0, (vaddr_t)tf->tf_a1, &retval);
			break;
		case SYS_sigaction:
			err = sys_sigaction((int)tf->tf_a0, (const_userptr_t)tf->tf_a1, (userptr_t)tf->tf_a2, &retval);
			break;
		case SYS_sigaltstack:
			err = sys_sigaltstack((const_userptr_t)tf->tf_a0, (userptr_t)tf->tf_a1, &retval);
			break;
		case SYS_sigprocmask:
			err = sys_sigprocmask((int)tf->tf_a0, (const_userptr_t)tf->tf_a1, (userptr_t)tf->tf_a2, &retval);
			break;
		case SYS_pause:
			err = sys_pause(&retval);
			break;
		case SYS_sigpending:
			err = sys_sigpending((userptr_t)tf->tf_a0, &retval);
			break;
		case SYS_sigsuspend:
			err = sys_sigsuspend((const_userptr_t)tf->tf_a0, &retval);
			break;
		case SYS_sigret:
			err = sys_sigreturn(tf, (userptr_t)tf->tf_a0);
			if (!err) {
				KASSERT(curthread->t_curspl == 0);
				KASSERT(curthread->t_iplhigh_count == 0);
				return;
			} else {
				KASSERT(0);
			}
		case SYS__exit:
			sys_exit((int)tf->tf_a0); // exits current user process, switches to new thread
			panic("shouldn't return from exit");
			break;
		case SYS_sleep:
			err = sys_sleep((int)tf->tf_a0, &retval);
			break;
		case SYS_pipe:
			err = sys_pipe((userptr_t)tf->tf_a0, (size_t)tf->tf_a1, &retval);
			break;
		case SYS_select:
			err = sys_select((int)tf->tf_a0, (userptr_t)tf->tf_a1, (userptr_t)tf->tf_a2, (userptr_t)tf->tf_a3,
				(userptr_t)((struct timeval*)tf->tf_sp+8), &retval);
			break;
		case SYS_socket:
			err = sys_socket((int)tf->tf_a0, (int)tf->tf_a1, (int)tf->tf_a2, &retval);
			break;
		case SYS_bind:
			err = sys_bind((int)tf->tf_a0, (userptr_t)tf->tf_a1, (socklen_t)tf->tf_a2, &retval);
			break;
		case SYS_listen:
			err = sys_listen((int)tf->tf_a0, (int)tf->tf_a1, &retval);
			break;
		case SYS_accept:
			err = sys_accept((int)tf->tf_a0, (userptr_t)tf->tf_a1, (userptr_t)tf->tf_a2, &retval);
			break;
		// system calls for testing Kernel
		case SYS_pageout_region:
			err = sys_pageout_region((uint32_t)tf->tf_a0, (size_t)tf->tf_a1, &retval);
			break;
		case SYS_lock_region:
			err = sys_lock_region((uint32_t)tf->tf_a0, (size_t)tf->tf_a1, &retval);
			break;
	  default:
			kprintf("Unknown syscall %d\n", callno);
			err = ENOSYS;
			break;
	}


	if (err) {
		/*
		 * Return the error code. This gets converted at
		 * userlevel to a return value of -1 and the error
		 * code in errno.
		 */
		tf->tf_v0 = err;
		tf->tf_a3 = 1;      /* signal an error */
	}
	else {
		/* Success. */
		tf->tf_v0 = retval;
		tf->tf_a3 = 0;      /* signal no error */
	}

	/*
	 * Now, advance the program counter, to avoid restarting
	 * the syscall over and over again.
	 */

	tf->tf_epc += 4;

	/* Make sure the syscall code didn't forget to lower spl */
	KASSERT(curthread->t_curspl == 0);
	/* ...or leak any spinlocks */
	KASSERT(curthread->t_iplhigh_count == 0);
}

/*
 * Enter user mode for a newly forked process.
 */
void enter_forked_process(void *tf, unsigned long unused) {
	(void)unused;
	struct thread *t = curthread;
	KASSERT(t);
	KASSERT(tf != NULL);

	// trapframe must be on currently running thread's stack, so we copy it over and rely on
	// it not getting popped from the parent's stack before this runs.
	struct trapframe tf_on_stack;
	//struct trapframe *tf_p = (struct trapframe *)tf;
	// FIXME: possible reference to invalid memory if parent pops its trapframe
	tf_on_stack = *(struct trapframe*)tf;
	//memcpy(&tf_on_stack, tf_p, sizeof(struct trapframe));

	/*
	 * Succeed and return 0.
	 */
	tf_on_stack.tf_v0 = 0;
	tf_on_stack.tf_a3 = 0;

	/*
	 * Advance the PC.
	 */
	tf_on_stack.tf_epc += 4;

	t->t_state = S_RUN;
	mips_usermode(&tf_on_stack);
}
