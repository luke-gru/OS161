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

#include <argvdata.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <thread.h>
#include <copyinout.h>
#include <spl.h>

struct argvdata *argvdata_create(void) {
	struct argvdata *args = kmalloc(sizeof(struct argvdata));
	bzero(args, sizeof(struct argvdata));
	return args;
}

void argvdata_destroy(struct argvdata *argdata) {
	if (argdata->buffer) {
		kfree(argdata->buffer);
	}
	argdata->bufend = NULL;
	argdata->buffer = NULL;
	if (argdata->offsets) {
		kfree(argdata->offsets);
	}
	argdata->offsets = NULL;
	argdata->nargs = 0;
	kfree(argdata);
}

void argvdata_debug(struct argvdata *args, const char *msg, char *progname) {
	DEBUG(DB_SYSCALL, "%s (%s) argv data:\n", msg, progname);
	if (args->nargs == 0) {
		DEBUG(DB_SYSCALL, "  argdata is empty!\n");
		return;
	}
	if (!args->buffer || !args->offsets || !args->bufend) {
		DEBUG(DB_SYSCALL, "  argdata is corrupted!\n");
		return;
	}
	args->offsets[0] = 0;
	for (int i = 0; i < args->nargs; i++) {
		char *arg = args->buffer + args->offsets[i];
		if (arg == NULL) {
			DEBUG(DB_SYSCALL, "  argdata has invalid arg #%d, is NULL!\n", i);
			continue;
		}
		DEBUG(DB_SYSCALL, "  arg%d: \"%s\"\n", i, arg);
	}
}

int argvdata_fill(struct argvdata *argdata, char *progname, char **args, int argc) {
	argdata->offsets = kmalloc(sizeof(size_t) * argc);
	argdata->offsets[0] = 0;
	size_t buflen = 0;
	for (int i = 0; i < argc; i++) {
		if (!args[i]) break;
		//kprintf("argvdata fill %d: '%s'\n", i, args[i]);
		if (i == 0) {
			KASSERT(strcmp(progname, args[i]) == 0);
		}

		buflen += strlen(args[i]) + 1; // add 1 for NULL character
	}
	buflen+=1; // NULL byte to end args array
	argdata->buffer = kmalloc(buflen);
	char *bufp = argdata->buffer;
	// copy args into buffer
	for (int i = 0; i < argc; i++) {
		size_t arg_sz = strlen(args[i]) + 1; // with terminating NULL
		memcpy(bufp, (const void *)args[i], arg_sz);
		KASSERT(strcmp(bufp, args[i]) == 0);
		if (i > 0) {
			argdata->offsets[i] = bufp - argdata->buffer;
		}
		KASSERT((argdata->buffer + argdata->offsets[i]) == bufp);
		bufp += arg_sz + 1;
	}
	argdata->bufend = argdata->buffer + buflen + 1;
	argdata->nargs = argc;
	return 0;
}

int argvdata_fill_from_uspace(struct argvdata *argdata, char *progname, userptr_t argv) {
	char **argv_p = (char**)argv;
	int nargs_given = 0;
	size_t buflen = 0;
	char *args[NARGS_MAX];
	char argbuf[ARG_SINGLE_MAX+1];
	bzero(args, NARGS_MAX * sizeof(char*));
	bzero(argbuf, ARG_SINGLE_MAX+1);
	args[0] = kstrdup(progname); // default
	for (int i = 0; argv_p[i] != 0 && i < NARGS_MAX; i++) {
			size_t arglen_got = 0;
			int copy_res = copyinstr((const_userptr_t)argv_p[i], argbuf, ARG_SINGLE_MAX, &arglen_got);
			if (copy_res != 0) {
				panic("invalid copy for arg #%d: (%s)", i, argv_p[i]); // FIXME:
			}
			args[i] = (char*)kmalloc(arglen_got); // includes NULL byte, freed further down
			memcpy(args[i], argbuf, arglen_got);
			bzero(argbuf, ARG_SINGLE_MAX+1);
			nargs_given++;
			buflen += arglen_got;
	}
	buflen+=1; // NULL byte to end args array
	argdata->buffer = kmalloc(buflen);
	bzero(argdata->buffer, buflen);
	argdata->offsets = kmalloc(sizeof(size_t) * nargs_given);
	argdata->offsets[0] = 0;
	char *bufp = argdata->buffer;
	// move arg strings into buffer
	for (int i = 0; i < nargs_given; i++) {
		size_t arg_sz = strlen(args[i]) + 1; // with terminating NULL
		memcpy(bufp, (const void *)args[i], arg_sz);
		KASSERT(strcmp(bufp, args[i]) == 0);
		if (i > 0) {
			argdata->offsets[i] = bufp - argdata->buffer;
		}
		KASSERT((argdata->buffer + argdata->offsets[i]) == bufp);
		bufp += arg_sz + 1;
	}
	argdata->bufend = argdata->buffer + buflen + 1;
	argdata->nargs = nargs_given;
	return 0;
}

/*
 * copyout_args
 * copies the argv out of the kernel space argvdata into the userspace.
 * read through the comments to see how it works.
 */
static int copyout_args(struct argvdata *ad, userptr_t *argv, vaddr_t *stackptr) {
	userptr_t argbase, userargv, arg;
	vaddr_t stack;
	size_t buflen;
	int i, result;

	/* we use the buflen a lot, precalc it */
	buflen = ad->bufend - ad->buffer;

	/* begin the stack at the passed in top */
	stack = *stackptr;

	/*
	 * copy the block of strings to the top of the user stack.
	 * we can do it as one big blob.
	 */

	/* figure out where the strings start */
	stack -= buflen;

	/* align to sizeof(void *) boundary, this is the argbase */
	stack -= (stack & (sizeof(void *) - 1));
	argbase = (userptr_t)stack;

	/* now just copyout the whole block of arg strings  */
	result = copyout(ad->buffer, argbase, buflen);
	if (result) {
		return result;
	}

	/*
	 * now copy out the argv array itself.
	 * the stack pointer is already suitably aligned.
	 * allow an extra slot for the NULL that terminates the vector.
	 */
	stack -= (ad->nargs + 1)*sizeof(userptr_t);
	userargv = (userptr_t)stack;

	KASSERT(ad->offsets[0] == 0);
	for (i = 0; i < ad->nargs; i++) {
		arg = argbase + ad->offsets[i];
		result = copyout(&arg, userargv, sizeof(userptr_t));
		if (result) {
			return result;
		}
		userargv += sizeof(userptr_t);
	}

	/* NULL terminate it */
	arg = NULL;
	result = copyout(&arg, userargv, sizeof(userptr_t));
	if (result) {
		return result;
	}

	*argv = (userptr_t)stack;
	*stackptr = stack;
	return 0;
}


/*
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Calls vfs_open on progname and thus may destroy it.
 */
int runprogram(char *progname, char **args, int nargs) {
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	if (curproc == kproc) {
		panic("Can't run runprogram on the kernel itself!");
	}

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result != 0) {
		return result;
	}

	/* We should be a new process. */
	KASSERT(proc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	proc_setas(as);
	as_activate();

	/* Load the executable, setting fields of curproc->p_addrspace. */
	result = load_elf(v, &entrypoint);
	if (result != 0) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result != 0) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}

	struct argvdata *argdata = argvdata_create();
	argvdata_fill(argdata, progname, args, nargs);
	argvdata_debug(argdata, "runprogram", progname);
	userptr_t userspace_argv_ary;
	if (copyout_args(argdata, &userspace_argv_ary, &stackptr) != 0) {
		DEBUG(DB_SYSCALL, "Error copying args into user process\n");
		argvdata_destroy(argdata);
		return -1;
	}
	argvdata_destroy(argdata);
	int pre_exec_res;
	if ((pre_exec_res = proc_pre_exec(curproc, progname)) != 0) {
		return pre_exec_res;
	}
	/* Warp to user mode. */
	enter_new_process(nargs /*argc*/, userspace_argv_ary /*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

/*
Like runprogram, but loads argv from current process's address space for use
with the execv system call.
TODO: I think we're leaking kernel thread stack space each time we do an execv, because
curthread->t_stack isn't freed and then reallocated. If we do this, we need interrupts
* to be disabled so we aren't pre-empted and return to an invalid stack.
*/
int	runprogram_uspace(char *progname, struct argvdata *argdata) {
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	if (curproc == kproc) {
		panic("Can't run runprogram_uspace (execv) on the kernel itself!");
	}

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result != 0) {
		return result;
	}

	/* We should be a new process or one that we just wiped */
	KASSERT(proc_getas() == NULL);

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	userptr_t userspace_argv_ary;
	proc_setas(as);
	as_activate();
	/* Load the executable, setting fields of curproc->p_addrspace. */
	result = load_elf(v, &entrypoint);
	if (result != 0) {
		as_destroy(as);
		vfs_close(v);
		return result;
	}
	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result != 0) {
		as_destroy(as);
		return result;
	}
	int copyout_res = copyout_args(argdata, &userspace_argv_ary, &stackptr);
	if (copyout_res != 0) {
		DEBUG(DB_SYSCALL, "Error copying args during exec\n");
		if (copyout_res)
			panic("debug"); // FIXME:remove
		as_destroy(as);
		return copyout_res;
	}
	int nargs = argdata->nargs;

	/* Done with the file now. */
	vfs_close(v);

	int pre_exec_res;
	if ((pre_exec_res = proc_pre_exec(curproc, progname)) != 0) {
		as_destroy(as);
		return pre_exec_res;
	}

	// DEBUG(DB_SYSCALL, "sys_execv entering new process %d\n", curproc->pid);
	spl0();
	/* Warp to user mode. */
	enter_new_process(nargs /*argc*/, userspace_argv_ary /*userspace addr of argv*/,
				NULL /*userspace addr of environment*/,
				stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}
