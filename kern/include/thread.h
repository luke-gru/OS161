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

#ifndef _THREAD_H_
#define _THREAD_H_

/*
 * Definition of a thread.
 *
 * Note: curthread is defined by <current.h>.
 */

#include <array.h>
#include <spinlock.h>
#include <threadlist.h>
#include <signal.h>

struct cpu;
struct trapframe;

/* get machine-dependent defs */
#include <machine/thread.h>


/* Size of kernel stacks; must be power of 2 */
#define STACK_SIZE 4096
#define MAX_NAME_LENGTH 64

/* Mask for extracting the stack base address of a kernel stack pointer */
#define STACK_MASK  (~(vaddr_t)(STACK_SIZE-1))

/* Macro to test if two addresses are on the same kernel stack */
#define SAME_STACK(p1, p2)     (((p1) & STACK_MASK) == ((p2) & STACK_MASK))


/* States a thread can be in. */
typedef enum {
	S_RUN,		/* running */
	S_READY,	/* ready to run */
	S_SLEEP,	/* sleeping, blocked, or stopped */
	S_ZOMBIE,	/* zombie; exited but not yet deleted */
} threadstate_t;

const char *threadstate_name(threadstate_t state);

/* Thread structure. */
struct thread {
	/*
	 * These go up front so they're easy to get to even if the
	 * debugger is messed up.
	 */

	char t_name[MAX_NAME_LENGTH];
	const char *t_wchan_name;	/* Name of wait channel, if sleeping. Useful when debugging */
	struct wchan *t_wchan;
	struct spinlock *t_wchan_lk;
	threadstate_t t_state;		/* State this thread is in */
	/* when running a signal handler, we sometimes need to save the previous threadstate
	 * For instance, when we send a SIGSTOP to a process that's currently sleeping,
	 * t_state becomes S_STOPPED and t_prevstate becomes S_SLEEP
	 */
	//threadstate_t t_prevstate;


	/*
	 * Thread subsystem internal fields.
	 */
	struct thread_machdep t_machdep; /* Any machine-dependent goo */
	struct threadlistnode t_listnode; /* Link for run/sleep/zombie lists. Can only be on 1 of these lists at a time! */
	void *t_stack;			/* Kernel-level stack for thread */
	struct switchframe *t_context;	/* Saved register context from context switch (on stack) */
	struct cpu *t_cpu;		/* CPU thread runs on. NOTE: threads can migrate between CPUs (see thread_consider_migration) */
	struct proc *t_proc;		/* Process thread belongs to. Aside from kernel threads that don't have processes and kswapd, these are userspace processes */
	time_t wakeup_at; /* for timed wakeup (userland sleep(2) syscall) */
	/* Same as t_proc->pid, but proc might be destroyed before thread is cleaned up, so we store it here too.
	 * Right now, all userspace processes run in their own threads, and new userspace threads (using clone() syscall)
	 * also create new processes with their pids and their own thread structure.
	 */
	pid_t t_pid;
	HANGMAN_ACTOR(t_hangman);	/* Deadlock detector hook */

	/*
	 * Interrupt state fields.
	 *
	 * t_in_interrupt is true if current execution is in an
	 * interrupt handler, which means the thread's normal context
	 * of execution is stopped somewhere in the middle of doing
	 * something else. This makes assorted operations unsafe.
	 *
	 * See notes in spinlock.c regarding t_curspl and t_iplhigh_count.
	 *
	 * Exercise for the student: why is this material per-thread
	 * rather than per-cpu or global?
	 */
	bool t_in_interrupt;		/* Are we in an interrupt? */
	int t_curspl;			/* Current spl*() state */
	int t_iplhigh_count;		/* # of times IPL has been raised */

	/*
	 * Public fields
	 */

	struct siginfo *t_pending_signals[PENDING_SIGNALS_MAX];
	bool t_is_stopped; /* has been stopped by SIGSTOP */
	bool t_is_paused; /* has been paused by pause() syscall (unpauses after receiving a signal)*/
	sigset_t t_sigmask;
};

/*
 * Array of threads.
 */
#ifndef THREADINLINE
#define THREADINLINE INLINE
#endif

// threadarray_get, threadarray_num
DECLARRAY(thread, THREADINLINE);
DEFARRAY(thread, THREADINLINE);

/* Call once during system startup to allocate data structures. */
void thread_bootstrap(void);

/* Call late in system startup to get secondary CPUs running. */
void thread_start_cpus(void);

/* Call during panic to stop other threads in their tracks */
void thread_panic(void);

/* Call during system shutdown to offline other CPUs. */
void thread_shutdown(void);

void thread_stop(void);

/*
 * Make a new thread, which will start executing at "func". The thread
 * will belong to the process "proc", or to the current thread's
 * process if "proc" is null. The "data" arguments (one pointer, one
 * number) are passed to the function. The current thread is used as a
 * prototype for creating the new one. Returns an error code. The
 * thread structure for the new thread is not returned; it is not in
 * general safe to refer to it as the new thread may exit and
 * disappear at any time without notice.
 */
int thread_fork(const char *name, struct proc *proc,
                void (*entrypoint)(void *data1, unsigned long data2),
								void *data1, unsigned long data2);
// same as above except starts on the given CPU
int thread_fork_in_cpu(const char *name, struct proc *proc, struct cpu *cpu,
									     void (*entrypoint)(void *data1, unsigned long data2),
										 	 void *data1, unsigned long data2);
int thread_fork_for_clone(struct thread *parent_th, struct proc *clone,
											 	  userptr_t entrypoint, void *data1, int *errcode);
struct cpu *thread_get_cpu(unsigned index);
int thread_fork_from_proc(struct thread *th, struct proc *pr, struct trapframe *tf, int *errcode);

struct thread *thread_find_by_id(pid_t id);
int thread_send_signal(struct thread *t, int sig);
int thread_has_pending_signal(void);
int thread_remove_pending_signal(unsigned sigidx, struct siginfo **siginfo_out);
int thread_handle_signal(struct siginfo siginf);
/*
 * Cause the current thread to exit.
 * Interrupts need not be disabled.
 */
void thread_exit(int status);

/*
 * Cause the current thread to yield to the next runnable thread, but
 * itself stay runnable.
 * Interrupts need not be disabled.
 */
void thread_yield(void);

/*
 * Reshuffle the run queue. Called from the timer interrupt.
 */
void schedule(void);

void exorcise(void); // clean up zombie threads

/*
 * Potentially migrate ready threads to other CPUs. Called from the
 * timer interrupt.
 */
void thread_consider_migration(void);

extern unsigned thread_count;
void thread_wait_for_count(unsigned);

void thread_sleep_n_seconds(int seconds);
void thread_add_ready_sleepers_to_runqueue(void);

int wchan_wake_thread(struct wchan *wc, struct spinlock *lk, struct thread *t);

#endif /* _THREAD_H_ */
