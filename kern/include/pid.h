/*
 * Process ID managment.
 */

#ifndef _PID_H_
#define _PID_H_

#define INVALID_PID	0	/* nothing has this pid */
#define BOOTUP_PID	1	/* first thread has this pid */

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
 * Set the exit status of the current process to status.  Wakes up any threads
 * waiting to read this status, and decrefs the current thread's pid.
 */
void pid_setexitstatus(int status);

/*
 * Causes the current thread to wait for the process with pid `targetpid` to
 * exit, returning the exit status when it does.
 */
int pid_wait(pid_t targetpid, int *status);

#endif /* _PID_H_ */
