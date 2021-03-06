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
#include <kern/reboot.h>
#include <kern/unistd.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <limits.h>
#include <lib.h>
#include <uio.h>
#include <clock.h>
#include <mainbus.h>
#include <synch.h>
#include <thread.h>
#include <proc.h>
#include <vfs.h>
#include <sfs.h>
#include <syscall.h>
#include <addrspace.h>
#include <test.h>
#include <prompt.h>
#include <current.h>
#include "opt-sfs.h"
#include "opt-net.h"
#include "opt-synchprobs.h"
#include "opt-automationtest.h"
#include <lib.h>

/*
 * In-kernel menu and command dispatcher.
 */

#define _PATH_SHELL "/bin/sh"

#define MAXMENUARGS  16

////////////////////////////////////////////////////////////
//
// Command menu functions

/*
 * Function for a thread that runs an arbitrary userlevel program by
 * name.
 *
 * It copies the program name because runprogram destroys the copy
 * it gets by passing it to vfs_open().
 */
static
void
cmd_progthread(void *ptr, unsigned long nargs)
{
	char **args = ptr;
	char progname[128];
	int result;

	KASSERT(nargs >= 1);

	/* Hope we fit. */
	KASSERT(strlen(args[0]) < sizeof(progname));

	strcpy(progname, args[0]);

	result = runprogram(progname, args, nargs);
	if (result) {
		kprintf("Running program %s failed: %s\n", args[0], strerror(result));
		return;
	}
	panic("NOTREACHED");
}

/*
 * Common code for cmd_prog and cmd_shell.
 */
static
int
common_prog(int nargs, char **args, bool wait_for_exit)
{
	struct proc *proc;
	int result;

	/* Create a process for the new program to run in. */
	proc = proc_create_runprogram(args[0] /* name */);
	if (proc == NULL) {
		return -1;
	}

	if (!wait_for_exit) {
		//proc_redir_standard_streams(proc, FD_DEV_NULL);
	}

	pid_t child_pid = 0;
	result = thread_fork(args[0] /* thread name */,
			proc /* new process */,
			cmd_progthread /* thread function */,
			args /* thread arg */, nargs /* thread arg */);
	if (result != 0) {
		kprintf("thread_fork failed: %s\n", strerror(result));
		proc_destroy(proc);
		return -1;
	}
	// FIXME: careful, proc could be destroyed by child thread upon exit, in which case
	// this memory access is invalid
	child_pid = proc->pid;
	if (!wait_for_exit) {
		thread_sleep_n_seconds(1); // HACK: make sure data we send to thread_fork isn't freed cleaned up before
		// cmd_progthread gets run
		return child_pid;
	}
	if (child_pid > 0) {
		int waiterr;
		result = proc_waitpid_sleep(child_pid, &waiterr);
		if (result == -1) {
			kprintf("waitpid error: %d\n", waiterr);
		} else if (result != 0) {
			kprintf("exitstatus: %d\n", result);
		}
	} else {
		panic("invalid child pid: %d", (int)child_pid);
	}

	DEBUG(DB_VM, "free core pages: %lu\n", corefree());
	return 0;
}

/*
 * Command for running an arbitrary userlevel program.
 */
static
int
cmd_prog(int nargs, char **args)
{
	if (nargs < 2) {
		kprintf("Usage: p program [arguments]\n");
		return EINVAL;
	}

	/* drop the leading "p" */
	args++;
	nargs--;

	return common_prog(nargs, args, true);
}

/*
 * Command for running an arbitrary userlevel program.
 */
static
int
cmd_prog_background(int nargs, char **args)
{
	if (nargs < 2) {
		kprintf("Usage: b program [arguments]\n");
		return EINVAL;
	}

	/* drop the leading "b" */
	args++;
	nargs--;

	int pid = common_prog(nargs, args, false);
	if (pid < 3) {
		return -1;
	}
	kprintf("pid: %d\n", pid);
	return 0;
}

static int cmd_sig(int nargs, char **args) {
	if (nargs != 3) {
		kprintf("Usage: sig SIGNAME PID\n");
		return EINVAL;
	}
	int sig;
	int pid;
	if (strcmp(args[1], "SIGSTOP") == 0) {
		sig = SIGSTOP;
	} else if (strcmp(args[1], "SIGCONT") == 0) {
		sig = SIGCONT;
	} else if (strcmp(args[1], "SIGKILL") == 0) {
		sig = SIGKILL;
	} else if (strcmp(args[1], "SIGUSR1") == 0) {
		sig = SIGUSR1;
	} else if (strcmp(args[1], "SIGTERM") == 0) {
		sig = SIGTERM;
	} else {
		kprintf("Only SIGSTOP, SIGCONT, SIGKILL, SIGTERM, SIGUSR1 are supported\n");
		return EINVAL;
	}
	pid = atoi(args[2]);
	if (pid <= 2) {
		kprintf("Invalid PID\n");
		return EINVAL;
	}
	if (pid == (int)curproc->pid) {
		kprintf("Can't send signal to shell process\n");
		return EINVAL;
	}
	int errcode = 0;
	struct proc *p = proc_lookup((pid_t)pid);
	if (!p) {
		kprintf("Process %d not found\n", pid);
		return ESRCH;
	}
	int res = proc_send_signal(p, sig, NULL, &errcode);
	if (res == -1) {
		kprintf("Error sending signal\n");
		return errcode;
	} else if (res == SIG_ISBLOCKED) {
		kprintf("Signal blocked\n");
	} else if (res == 0) {
		kprintf("Successfully sent signal\n");
	} else {
		KASSERT(0); // unknown return value
	}
	return 0;
}

static int cmd_list_processes(int nargs, char **args) {
	if (nargs != 1) {
		kprintf("Usage: ps\n");
		return EINVAL;
	}
	(void)args;
	proc_list();
	return 0;
}

/*
 * Command for starting the system shell.
 */
static
int
cmd_shell(int nargs, char **args)
{
	(void)args;
	if (nargs != 1) {
		kprintf("Usage: s\n");
		return EINVAL;
	}

	args[0] = (char *)_PATH_SHELL;

	return common_prog(nargs, args, true);
}

/*
 * Command for changing directory.
 */
static
int
cmd_chdir(int nargs, char **args)
{
	if (nargs != 2) {
		kprintf("Usage: cd directory\n");
		return EINVAL;
	}

	return vfs_chdir(args[1]);
}

/*
 * Command for printing the current directory.
 */
static
int
cmd_pwd(int nargs, char **args)
{
	char buf[PATH_MAX+1];
	int result;
	struct iovec iov;
	struct uio ku;

	(void)nargs;
	(void)args;

	uio_kinit(&iov, &ku, buf, sizeof(buf)-1, 0, UIO_READ);
	result = vfs_getcwd(&ku);
	if (result) {
		kprintf("vfs_getcwd failed (%s)\n", strerror(result));
		return result;
	}

	/* null terminate */
	buf[sizeof(buf)-1-ku.uio_resid] = 0;

	/* print it */
	kprintf("%s\n", buf);

	return 0;
}

static int cmd_touch(int nargs, char **args) {
	if (nargs != 2) {
		kprintf("Usage: touch FILENAME\n");
		return EINVAL;
	}
	char *fname = args[1];
	int result;
	struct vnode *node;
	result = vfs_open(fname, O_WRONLY|O_CREAT|O_EXCL, 0644, &node);
	if (result != 0) {
		kprintf("vfs_open failed (%s)\n", strerror(result));
		return result;
	}
	int errcode = 0;
	struct filedes *new_filedes = filedes_open(curproc, fname, node, O_WRONLY|O_CREAT|O_EXCL, -1, &errcode);
	KASSERT(new_filedes);
	filedes_close(curproc, new_filedes);
	kprintf("successfully created file '%s'\n", fname);
	return 0;
}

static int cmd_append_line_to_file(int nargs, char **args) {
	if (nargs != 3) {
		kprintf("Usage: af FILENAME 'line to add'\n");
		return EINVAL;
	}
	char *fname = kstrdup(args[1]);
	char *line = kmalloc(strlen(args[2]) + 2); // for adding newline and terminating NULL
	memcpy(line, args[2], strlen(args[2])+1);
	strcat(line, "\n");
	//kprintf("line: '%s', len: %d\n", line, (int)strlen(line));

	int errcode = 0;
	int fd = file_open(fname, O_WRONLY|O_APPEND, 0644, &errcode);
	if (fd < 0) {
		kprintf("file_open failed (%s) [code: %d]\n", strerror(errcode), errcode);
		return errcode;
	}
	struct filedes *file_des = filetable_get(curproc, fd);

	struct iovec iov;
	struct uio myuio;
	uio_kinit(&iov, &myuio, line, strlen(line), file_des->offset, UIO_WRITE);
	int res = file_write(file_des, &myuio, &errcode);
	if (res == -1) {
		kprintf("file_write failed (%s)\n", strerror(errcode));
		return errcode;
	}
	return 0;
}

/*
 * Command for running sync.
 */
static
int
cmd_sync(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	vfs_sync();

	return 0;
}

/*
 * Command for dropping to the debugger.
 */
static
int
cmd_debug(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	mainbus_debugger();

	return 0;
}

/*
 * Command for doing an intentional panic.
 */
static
int
cmd_panic(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	panic("User requested panic\n");
	return 0;
}

/*
 * Subthread for intentially deadlocking.
 */
struct deadlock {
	struct lock *lock1;
	struct lock *lock2;
};

static
void
cmd_deadlockthread(void *ptr, unsigned long num)
{
	struct deadlock *dl = ptr;

	(void)num;

	/* If it doesn't wedge right away, keep trying... */
	while (1) {
		lock_acquire(dl->lock2);
		lock_acquire(dl->lock1);
		kprintf("+");
		lock_release(dl->lock1);
		lock_release(dl->lock2);
	}
}

/*
 * Command for doing an intentional deadlock.
 */
static
int
cmd_deadlock(int nargs, char **args)
{
	struct deadlock dl;
	int result;

	(void)nargs;
	(void)args;

	dl.lock1 = lock_create("deadlock1");
	if (dl.lock1 == NULL) {
		kprintf("lock_create failed\n");
		return ENOMEM;
	}
	dl.lock2 = lock_create("deadlock2");
	if (dl.lock2 == NULL) {
		lock_destroy(dl.lock1);
		kprintf("lock_create failed\n");
		return ENOMEM;
	}

	result = thread_fork(args[0] /* thread name */,
			NULL /* kernel thread */,
			cmd_deadlockthread /* thread function */,
			&dl /* thread arg */, 0 /* thread arg */);
	if (result) {
		kprintf("thread_fork failed: %s\n", strerror(result));
		lock_release(dl.lock1);
		lock_destroy(dl.lock2);
		lock_destroy(dl.lock1);
		return result;
	}

	/* If it doesn't wedge right away, keep trying... */
	while (1) {
		lock_acquire(dl.lock1);
		lock_acquire(dl.lock2);
		kprintf(".");
		lock_release(dl.lock2);
		lock_release(dl.lock1);
	}
	/* NOTREACHED */
	return 0;
}

/*
 * Command for shutting down.
 */
static
int
cmd_quit(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	vfs_sync();
	sys_reboot(RB_POWEROFF);
	thread_exit(0);
	return 0;
}

/*
 * Command for mounting a filesystem.
 */

/* Table of mountable filesystem types. */
static const struct {
	const char *name;
	int (*func)(const char *device);
} mounttable[] = {
#if OPT_SFS
	{ "sfs", sfs_mount },
#endif
};

static
int
cmd_mount(int nargs, char **args)
{
	char *fstype;
	char *device;
	int i = 0;

	if (nargs != 3) {
		kprintf("Usage: mount fstype device:\n");
		return EINVAL;
	}

	fstype = args[1];
	device = args[2];

	/* Allow (but do not require) colon after device name */
	if (device[strlen(device)-1]==':') {
		device[strlen(device)-1] = 0;
	}

	for (i=0; i < (int)ARRAYCOUNT(mounttable); i++) {
		if (!strcmp(mounttable[i].name, fstype)) {
			return mounttable[i].func(device);
		}
	}
	kprintf("Unknown filesystem type %s\n", fstype);
	return EINVAL;
}

static
int
cmd_unmount(int nargs, char **args)
{
	char *device;

	if (nargs != 2) {
		kprintf("Usage: unmount device:\n");
		return EINVAL;
	}

	device = args[1];

	/* Allow (but do not require) colon after device name */
	if (device[strlen(device)-1]==':') {
		device[strlen(device)-1] = 0;
	}

	return vfs_unmount(device);
}

/*
 * Command to set the "boot fs".
 *
 * The boot filesystem is the one that pathnames like /bin/sh with
 * leading slashes refer to.
 *
 * The default bootfs is "emu0".
 */
static
int
cmd_bootfs(int nargs, char **args)
{
	char *device;

	if (nargs != 2) {
		kprintf("Usage: bootfs device\n");
		return EINVAL;
	}

	device = args[1];

	/* Allow (but do not require) colon after device name */
	if (device[strlen(device)-1]==':') {
		device[strlen(device)-1] = 0;
	}

	return vfs_setbootfs(device);
}

static
int
cmd_kheapstats(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	kheap_printstats();

	return 0;
}

static
int
cmd_kheapused(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	kheap_printused();

	return 0;
}

static
int
cmd_kheapgeneration(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	kheap_nextgeneration();

	return 0;
}

static
int
cmd_kheapdump(int nargs, char **args)
{
	if (nargs == 1) {
		kheap_dump();
	}
	else if (nargs == 2 && !strcmp(args[1], "all")) {
		kheap_dumpall();
	}
	else {
		kprintf("Usage: khdump [all]\n");
	}

	return 0;
}

////////////////////////////////////////
//
// Menus.

static
void
showmenu(const char *name, const char *x[])
{
	int ct, half, i;

	kprintf("\n");
	kprintf("%s\n", name);

	for (i=ct=0; x[i]; i++) {
		ct++;
	}
	half = (ct+1)/2;

	for (i=0; i<half; i++) {
		kprintf("    %-36s", x[i]);
		if (i+half < ct) {
			kprintf("%s", x[i+half]);
		}
		kprintf("\n");
	}

	kprintf("\n");
}

static const char *opsmenu[] = {
	"[s]       Shell                     ",
	"[p]       Other program             ",
	"[mount]   Mount a filesystem        ",
	"[unmount] Unmount a filesystem      ",
	"[bootfs]  Set \"boot\" filesystem     ",
	"[pf]      Print a file              ",
	"[cd]      Change directory          ",
	"[pwd]     Print current directory   ",
	"[touch]   Create new file           ",
	"[af]      Add line to file          ",
	"[sync]    Sync filesystems          ",
	"[debug]   Drop to debugger          ",
	"[panic]   Intentional panic         ",
	"[deadlock] Intentional deadlock     ",
	"[q]       Quit and shut down        ",
	NULL
};

static
int
cmd_opsmenu(int n, char **a)
{
	(void)n;
	(void)a;

	showmenu("OS/161 operations menu", opsmenu);
	return 0;
}

static const char *testmenu[] = {
	"[at]  Array test                    ",
	"[at2] Large array test              ",
	"[bt]  Bitmap test                   ",
	"[tlt] Threadlist test               ",
	"[km1] Kernel malloc test            ",
	"[km2] kmalloc stress test           ",
	"[km3] Large kmalloc test            ",
	"[km4] Multipage kmalloc test        ",
	"[km5] kmalloc coremap alloc test    ",
	"[tt1] Thread test 1                 ",
	"[tt2] Thread test 2                 ",
	"[tt3] Thread test 3                 ",
#if OPT_NET
	"[net] Network test                  ",
#endif
	"[sem1] Semaphore test               ",
	"[lt1]  Lock test 1           (1)    ",
	"[lt2]  Lock test 2           (1*)   ",
	"[lt3]  Lock test 3           (1*)   ",
	"[lt4]  Lock test 4           (1*)   ",
	"[lt5]  Lock test 5           (1*)   ",
	"[cvt1] CV test 1             (1)    ",
	"[cvt2] CV test 2             (1)    ",
	"[cvt3] CV test 3             (1*)   ",
	"[cvt4] CV test 4             (1*)   ",
	"[cvt5] CV test 5             (1)    ",
	"[rwt1] RW lock test          (1?)   ",
	"[rwt2] RW lock test 2        (1?)   ",
	"[rwt3] RW lock test 3        (1?)   ",
	"[rwt4] RW lock test 4        (1?)   ",
	"[rwt5] RW lock test 5        (1?)   ",
#if OPT_SYNCHPROBS
	"[sp1] Whalemating test       (1)    ",
	"[sp2] Stoplight test         (1)    ",
#endif
	"[semu1-22] Semaphore unit tests     ",
	"[fs1] Filesystem test               ",
	"[fs2] FS read stress                ",
	"[fs3] FS write stress               ",
	"[fs4] FS write stress 2             ",
	"[fs5] FS long stress                ",
	"[fs6] FS create stress              ",
	"[hm1] HMAC unit test                ",
	NULL
};

static
int
cmd_testmenu(int n, char **a)
{
	(void)n;
	(void)a;

	showmenu("OS/161 tests menu", testmenu);
	kprintf("    (1) These tests will fail until you finish the "
		"synch assignment.\n");
	kprintf("    (*) These tests will panic on success.\n");
	kprintf("    (?) These tests are left to you to implement.\n");
	kprintf("\n");

	return 0;
}

#if OPT_AUTOMATIONTEST
static const char *automationmenu[] = {
	"[dl]   Deadlock test (*)             ",
	"[ll1]  Livelock test (1 thread)      ",
	"[ll16] Livelock test (16 threads)     ",
	NULL
};

static
int
cmd_automationmenu(int n, char **a)
{
	(void)n;
	(void)a;

	showmenu("OS/161 automation tests menu", automationmenu);
	kprintf("    (*) These tests require locks.\n");
	kprintf("\n");

	return 0;
}
#endif

static const char *mainmenu[] = {
	"[?o] Operations menu                ",
	"[?t] Tests menu                     ",
	"[kh] Kernel heap stats              ",
	"[khu] Kernel heap usage             ",
	"[khgen] Next kernel heap generation ",
	"[khdump] Dump kernel heap           ",
	"[q] Quit and shut down              ",
	NULL
};

static
int
cmd_mainmenu(int n, char **a)
{
	(void)n;
	(void)a;

	showmenu("OS/161 kernel menu", mainmenu);
	return 0;
}

////////////////////////////////////////
//
// Command table.

static struct {
	const char *name;
	int (*func)(int nargs, char **args);
} cmdtable[] = {
	/* menus */
	{ "?",		cmd_mainmenu },
	{ "h",		cmd_mainmenu },
	{ "help",	cmd_mainmenu },
	{ "?o",		cmd_opsmenu },
	{ "?t",		cmd_testmenu },
#if OPT_AUTOMATIONTEST
	{ "?a",		cmd_automationmenu },
#endif

	/* operations */
	{ "s",		cmd_shell },
	{ "p",		cmd_prog },
	{ "b",     cmd_prog_background },
	{ "sig",   cmd_sig },
	{ "ps",     cmd_list_processes },
	{ "mount",	cmd_mount },
	{ "unmount",	cmd_unmount },
	{ "bootfs",	cmd_bootfs },
	{ "pf",		printfile },
	{ "touch", cmd_touch },
	{ "af", cmd_append_line_to_file },
	{ "cd",		cmd_chdir },
	{ "pwd",	cmd_pwd },
	{ "sync",	cmd_sync },
	{ "debug",	cmd_debug },
	{ "panic",	cmd_panic },
	{ "deadlock",	cmd_deadlock },
	{ "q",		cmd_quit },
	{ "exit",	cmd_quit },
	{ "halt",	cmd_quit },

	/* stats */
	{ "kh",         cmd_kheapstats },
	{ "khu",        cmd_kheapused },
	{ "khgen",      cmd_kheapgeneration },
	{ "khdump",     cmd_kheapdump },

	/* base system tests */
	{ "at",		arraytest },
	{ "at2",	arraytest2 },
	{ "bt",		bitmaptest },
	{ "tlt",	threadlisttest },
	{ "km1",	kmalloctest },
	{ "km2",	kmallocstress },
	{ "km3",	kmalloctest3 },
	{ "km4",	kmalloctest4 },
	{ "km5",	kmalloctest5 },
#if OPT_NET
	{ "net",	nettest },
#endif
	{ "tt1",	threadtest },
	{ "tt2",	threadtest2 },
	{ "tt3",	threadtest3 },

	/* synchronization assignment tests */
	{ "sem1",	semtest },
	{ "lt1",	locktest },
	{ "lt2",	locktest2 },
	{ "lt3",	locktest3 },
	{ "lt4", 	locktest4 },
	{ "lt5", 	locktest5 },
	{ "cvt1",	cvtest },
	{ "cvt2",	cvtest2 },
	{ "cvt3",	cvtest3 },
	{ "cvt4",	cvtest4 },
	{ "cvt5",	cvtest5 },
	{ "rwt1",	rwtest },
	{ "rwt2",	rwtest2 },
	{ "rwt3",	rwtest3 },
	{ "rwt4",	rwtest4 },
	{ "rwt5",	rwtest5 },
#if OPT_SYNCHPROBS
	{ "sp1",	whalemating },
	{ "sp2",	stoplight },
#endif

	/* semaphore unit tests */
	{ "semu1",	semu1 },
	{ "semu2",	semu2 },
	{ "semu3",	semu3 },
	{ "semu4",	semu4 },
	{ "semu5",	semu5 },
	{ "semu6",	semu6 },
	{ "semu7",	semu7 },
	{ "semu8",	semu8 },
	{ "semu9",	semu9 },
	{ "semu10",	semu10 },
	{ "semu11",	semu11 },
	{ "semu12",	semu12 },
	{ "semu13",	semu13 },
	{ "semu14",	semu14 },
	{ "semu15",	semu15 },
	{ "semu16",	semu16 },
	{ "semu17",	semu17 },
	{ "semu18",	semu18 },
	{ "semu19",	semu19 },
	{ "semu20",	semu20 },
	{ "semu21",	semu21 },
	{ "semu22",	semu22 },

	/* file system assignment tests */
	{ "fs1",	fstest },
	{ "fs2",	readstress },
	{ "fs3",	writestress },
	{ "fs4",	writestress2 },
	{ "fs5",	longstress },
	{ "fs6",	createstress },

	/* HMAC unit tests */
	{ "hm1",	hmacu1 },

#if OPT_AUTOMATIONTEST
	/* automation tests */
	{ "dl",	dltest },
	{ "ll1",	ll1test },
	{ "ll16",	ll16test },
#endif

	{ NULL, NULL }
};

/*
 * Process a single command.
 */
static
int
cmd_dispatch(char *cmd)
{
	struct timespec before, after, duration;
	char *args[MAXMENUARGS];
	int nargs=0;
	char *word;
	char *context;
	int i, result;

	for (word = strtok_r(cmd, " \t", &context);
	     word != NULL;
	     word = strtok_r(NULL, " \t", &context)) {

		if (nargs >= MAXMENUARGS) {
			kprintf("Command line has too many words\n");
			return E2BIG;
		}
		args[nargs++] = word;
	}

	if (nargs==0) {
		return 0;
	}

	for (i=0; cmdtable[i].name; i++) {
		if (*cmdtable[i].name && !strcmp(args[0], cmdtable[i].name)) {
			KASSERT(cmdtable[i].func!=NULL);

			gettime(&before);

			result = cmdtable[i].func(nargs, args);

			gettime(&after);
			timespec_sub(&after, &before, &duration);

			kprintf("Operation took %llu.%09lu seconds\n",
				(unsigned long long) duration.tv_sec,
				(unsigned long) duration.tv_nsec);

			return result;
		}
	}

	kprintf("%s: Command not found\n", args[0]);
	return EINVAL;
}

/*
 * Evaluate a command line that may contain multiple semicolon-delimited
 * commands.
 *
 * If "isargs" is set, we're doing command-line processing; print the
 * comamnds as we execute them and panic if the command is invalid or fails.
 */
static
void
menu_execute(char *line, int isargs)
{
	char *command;
	char *context;
	int result;

	for (command = strtok_r(line, ";", &context);
	     command != NULL;
	     command = strtok_r(NULL, ";", &context)) {

		if (isargs) {
			kprintf("OS/161 kernel: %s\n", command);
		}

		result = cmd_dispatch(command);
		if (result) {
			kprintf("Menu command failed: %s\n", strerror(result));
			if (isargs) {
				panic("Failure processing kernel arguments\n");
			}
		}
	}
}

/*
 * Command menu main loop.
 *
 * First, handle arguments passed on the kernel's command line from
 * the bootloader. Then loop prompting for commands.
 *
 * The line passed in from the bootloader is treated as if it had been
 * typed at the prompt. Semicolons separate commands; spaces and tabs
 * separate words (command names and arguments).
 *
 * So, for instance, to mount an SFS on lhd0 and make it the boot
 * filesystem, and then boot directly into the shell, one would use
 * the kernel command line
 *
 *      "mount sfs lhd0; bootfs lhd0; s"
 */

void
menu(char *args)
{
	dbflags = DB_SYSCALL|DB_SIG|DB_UIO;
	kswapproc->p_cwd = kproc->p_cwd; // necessary for opening swap file
	char buf[64];
	menu_execute(args, 1);

	while (1) {
		/*
		 * Defined in overwrite.h. If you want to change the kernel prompt, please
		 * do it in that file. Otherwise automated test testing will break.
		 */
		kprintf(KERNEL_PROMPT);
		kgets(buf, sizeof(buf));
		menu_execute(buf, 0);
	}
}
