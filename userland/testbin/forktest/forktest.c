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

/*
 * forktest - test fork().
 *
 * This should work correctly when fork is implemented.
 *
 * It should also continue to work after subsequent assignments, most
 * notably after implementing the virtual memory system.
 */

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <test161/test161.h>

#define FORKTEST_FILENAME_BASE "forktest"

static char filename[32];

/*
 * This is used by all processes, to try to help make sure all
 * processes have a distinct address space.
 */
static volatile int mypid;

/*
 * Helper function for fork that prints a warning on error.
 */
static
int
dofork(void)
{
	int pid;
	pid = fork();
	printf("fork return val: %d\n", pid);
	if (pid < 0) {
		warn("fork failed from userland");
	}
	return pid;
}

/*
 * Check to make sure each process has its own address space. Write
 * the pid into the data segment and read it back repeatedly, making
 * sure it's correct every time.
 */
static
void
check(void)
{
	int i;

	mypid = getpid();

	/* Make sure each fork has its own address space. */
	nprintf(".");
	for (i=0; i<800; i++) {
		volatile int seenpid;
		seenpid = mypid;
		if (seenpid != getpid()) {
			errx(1, "pid mismatch iter %d (%d, should be %d) "
			     "- your vm is broken!",
			     i, seenpid, getpid());
		}
	}
}

/*
 * Wait for a child process.
 *
 * This assumes dowait is called the same number of times as dofork
 * and passed its results in reverse order. Any forks that fail send
 * us -1 and are ignored. The first 0 we see indicates the fork that
 * generated the current process; that means it's time to exit. Only
 * the parent of all the processes returns from the chain of dowaits.
 */
static
void
dowait(int nowait, int pid)
{
	int x = 1001; // exit status not set

	if (pid<0) {
		/* fork in question failed; just return */
		return;
	}
	if (pid==0) {
		/* in the fork in question we were the child; exit */
		printf("exiting from fork\n");
		exit(0);
	}
	printf("waiting on PID: %d\n", pid);
	int wait_ret = 0;
	if (!nowait) {
		if ((wait_ret = waitpid(pid, &x, 0)) < 0) {
			errx(1, "waitpid failed with return value: %d, error: %s", wait_ret, strerror(errno));
		}
		else if (WIFSIGNALED(x)) {
			errx(1, "pid %d: exitstatus: %d, signal %d", pid, x, WTERMSIG(x));
		}
		else if (WEXITSTATUS(x) != 0) {
			errx(1, "pid %d: exit %d", pid, WEXITSTATUS(x));
		}
	}
}

/*
 * Actually run the test.
 */
static
void
test(int nowait)
{
	int pid0, pid1, pid2, pid3;
	int depth = 0;

	/*
	 * Caution: This generates processes geometrically.
	 *
	 * It is unrolled to encourage gcc to registerize the pids,
	 * to prevent wait/exit problems if fork corrupts memory.
	 *
	 * Note: if the depth prints trigger and show that the depth
	 * is too small, the most likely explanation is that the fork
	 * child is returning from the write() inside putchar()
	 * instead of from fork() and thus skipping the depth++. This
	 * is a fairly common problem caused by races in the kernel
	 * fork code.
	 */

	/*
	 * Guru: We have a problem here.
	 * We need to write the output to a file since test161 is
	 * supposed to be as simple as possible. This requires the
	 * test to tell test161 whether it passed or succeeded.
	 * We cannot lseek and read stdout, to check the output,
	 * so we need to write the output to a file and then check it later.
	 *
	 * So far so good. However, if in the future, forktest is
	 * going to be combined with triple/quint/etc, then the filename
	 * cannot be the same across multiple copies. So we need a
	 * unique filename per instance of forktest.
	 * So...
	 */
	snprintf(filename, 32, "%s-%d.bin", FORKTEST_FILENAME_BASE, getpid());
	printf("printing to file: %s\n", filename);
	int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
	if(fd < 3) {
		// 0, 1, 2 are stdin, stdout, stderr
		err(1, "Failed to open file to write data into\n");
	}

	pid0 = dofork();
	nprintf(".");
	write(fd, "A", 1);
	depth++;
	if (depth != 1) {
		warnx("depth %d, should be 1", depth);
	}
	check();

	pid1 = dofork();
	nprintf(".");
	write(fd, "B", 1);
	depth++;
	if (depth != 2) {
		warnx("depth %d, should be 2", depth);
	}
	check();

	pid2 = dofork();
	nprintf(".");
	write(fd, "C", 1);
	depth++;
	if (depth != 3) {
		warnx("depth %d, should be 3", depth);
	}
	check();

	pid3 = dofork();
	nprintf(".");
	write(fd, "D", 1);
	depth++;
	if (depth != 4) {
		warnx("depth %d, should be 4", depth);
	}
	check();

	/*
	 * These must be called in reverse order to avoid waiting
	 * improperly.
	 */
	dowait(nowait, pid3);
	nprintf(".");
	dowait(nowait, pid2);
	nprintf(".");
	dowait(nowait, pid1);
	nprintf(".");
	dowait(nowait, pid0);
	nprintf(".");

	// Check if file contents are correct
	// lseek may not be implemented..so close and reopen
	int close_res = close(fd);
	if (close_res != 0) {
		err(1, "Failed to close file %s\n", filename);
	}
	fd = open(filename, O_RDONLY);
	if(fd < 3) {
		err(1, "Failed to open file for verification\n");
	}
	nprintf(".");

	char buffer[31];
	int len;

	memset(buffer, 0, 31);
	len = read(fd, buffer, 30);
	printf("\n%s\n", buffer);
	if(len != 30) {
		err(1, "Did not get expected number of characters from fd %d, got: %d, buffer: \n", fd, len, buffer);
	}
	nprintf(".");

	//Check if number of instances of each character is correct
	//2As; 4Bs; 8Cs; 16Ds
	int as, bs, cs, ds;
	as = 0;
	bs = 0;
	cs = 0;
	ds = 0;

	for(int i = 0; i < 30; i++) {
		// In C, char can be directly converted to an ASCII index
		// So, A is 65, B is 66, ...
		if(buffer[i] == 'A') {
			as++;
		} else if (buffer[i] == 'B') {
			bs++;
		} else if (buffer[i] == 'C') {
			cs++;
		} else if (buffer[i] == 'D') {
			ds++;
		} else {
			err(1, "invalid char: %c", buffer[i]);
		}
	}
	if(as != 2 || bs != 4 || cs != 8 || ds != 16) {
		// Failed
		err(1, "Failed! Expected 2'A', 4'B', 8'C', 16'D', got: %s\n", buffer);
	}
	nprintf("\n");
	success(TEST161_SUCCESS, SECRET, "/testbin/forktest");
	close(fd);
}

int
main(int argc, char *argv[])
{
	static const char expected[] =
		"|----------------------------|\n";
	int nowait=0;

	if (argc==2 && !strcmp(argv[1], "-w")) {
		nowait=1;
	}
	else if (argc!=1 && argc!=0) {
		warnx("usage: forktest [-w]");
		return 1;
	}
	warnx("Starting. Expect this many:");
	write(STDERR_FILENO, expected, strlen(expected));

	test(nowait);

	warnx("Complete.");
	return 0;
}
