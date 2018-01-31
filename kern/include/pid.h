/*
 * Process ID managment.
 */

#ifndef _PID_H_
#define _PID_H_

#define INVALID_PID	-1	/* nothing has this pid, or any negative value pid */
#define UNSET_PID    0  /* nothing has this pid */
#define BOOTUP_PID	 1	/* first kernel thread and process have this pid */
#define KSWAPD_PID   2 /* pid of kswapd */
#define USERPID_MIN  3

/*
 * Initialize pid management.
 */
void pid_bootstrap(void);

/*
 * Get a pid for a new process, and puts it in process id table. The current
 * process is the ppid of the pidinfo that's allocated.
 */
int pid_alloc(pid_t *retval);

/*
 * Undo pid_alloc (may blow up if the target has ever run)
 */
void pid_unalloc(pid_t targetpid);

/*
 * Disown a pid (abandon interest in its exit status)
 */
void pid_disown(pid_t targetpid);

/*
 * Set the exit status of the given PID's process to `status`.  Wakes up any threads
 * waiting to read this status.
 */
void pid_setexitstatus(pid_t pid, int status);

/*
 * Causes the current thread to wait for the process with pid `targetpid` to
 * exit, returning the exit status when it does.
 */
int pid_wait_sleep(pid_t targetpid, int *status);

bool is_valid_pid(pid_t pid); // NOTE: the kernel boot pid (1) and kswapd (2) are considered valid
bool is_valid_user_pid(pid_t pid); // NOTE: the kernel boot pid (1) and kswapd (2) are considered invalid here

#endif /* _PID_H_ */
