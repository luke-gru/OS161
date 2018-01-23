/*
 * Process ID management.
 * File new in SOL2.
 */

#include <types.h>
#include <kern/errno.h>
#include <limits.h> // PID_MIN, PID_MAX
#include <lib.h>
#include <thread.h>
#include <synch.h>
#include <pid.h>
#include <current.h>

/*
 * Structure for holding exit data of a thread.
 *
 * If pi_ppid is INVALID_PID, the parent has gone away and will not be
 * waiting. If pi_ppid is INVALID_PID and pi_exited is true, the
 * structure can be freed.
 */
struct pidinfo {
	int pi_pid;			// process id of this process
	int pi_ppid;			// process id of parent process
	volatile int pi_exited;		// true if process has exited
	int pi_exitstatus;		// status (only valid if exited)
	struct cv *pi_cv;		// use to wait for process exit
};


/*
 * Global pid and exit data.
 *
 * The process table is an el-cheapo hash table. It's indexed by
 * (pid % MAX_USERPROCS), and only allows one process per slot. If a
 * new pid allocation would cause a hash collision, we just don't
 * use that pid.
 */
static struct lock *pidlock;		// lock for global exit data
static struct pidinfo *pidinfo_ary[MAX_USERPROCS]; // actual pid info
static pid_t nextpid;			// next candidate pid
static int nprocs;			// number of allocated pids

/*
 * Create a pidinfo structure for the specified pid.
 */
static
struct pidinfo *
pidinfo_create(pid_t pid, pid_t ppid)
{
	struct pidinfo *pi;

	KASSERT(pid != INVALID_PID);

	pi = kmalloc(sizeof(struct pidinfo));
	if (pi==NULL) {
		return NULL;
	}

	pi->pi_cv = cv_create("pidinfo cv");
	if (pi->pi_cv == NULL) {
		kfree(pi);
		return NULL;
	}

	pi->pi_pid = pid;
	pi->pi_ppid = ppid;
	pi->pi_exited = 0;
	pi->pi_exitstatus = 0xbeef;  /* Recognizably invalid value */

	return pi;
}

/*
 * Clean up a pidinfo structure.
 */
static
void
pidinfo_destroy(struct pidinfo *pi)
{
	KASSERT(pi->pi_exited == 1);
	KASSERT(pi->pi_ppid == INVALID_PID);
	cv_destroy(pi->pi_cv);
	kfree(pi);
}

////////////////////////////////////////////////////////////

/*
 * pid_bootstrap: initialize.
 */
void
pid_bootstrap(void)
{
	int i;

	pidlock = lock_create("pidlock");
	if (pidlock == NULL) {
		panic("Out of memory creating pid lock\n");
	}

	/* not really necessary - should start zeroed */
	for (i=0; i<MAX_USERPROCS; i++) {
		pidinfo_ary[i] = NULL;
	}

	pidinfo_ary[BOOTUP_PID] = pidinfo_create(BOOTUP_PID, INVALID_PID);
	if (pidinfo_ary[BOOTUP_PID] == NULL) {
		panic("Out of memory creating bootup pid (init) data\n");
	}

	nextpid = PID_MIN;
	nprocs = 1;
}

/*
 * pi_get: look up a pidinfo in the process table.
 */
static
struct pidinfo *
pi_get(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(pid>=0);
	if (pid == INVALID_PID) return NULL;
	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo_ary[pid % MAX_USERPROCS];
	if (pi==NULL) {
		return NULL;
	}
	if (pi->pi_pid != pid) {
		return NULL;
	}
	return pi;
}

/*
 * pi_put: insert a new pidinfo in the process table. The right slot
 * must be empty.
 */
static
void
pi_put(pid_t pid, struct pidinfo *pi)
{
	KASSERT(lock_do_i_hold(pidlock));

	KASSERT(pid != INVALID_PID);

	KASSERT(pidinfo_ary[pid % MAX_USERPROCS] == NULL);
	pidinfo_ary[pid % MAX_USERPROCS] = pi;
	nprocs++;
}

/*
 * pi_drop: remove a pidinfo structure from the process table and free
 * it. It should reflect a process that has already exited and been
 * waited for.
 */
static
void
pi_drop(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo_ary[pid % MAX_USERPROCS];
	KASSERT(pi != NULL);
	KASSERT(pi->pi_pid == pid);

	pidinfo_destroy(pi);
	pidinfo_ary[pid % MAX_USERPROCS] = NULL;
	nprocs--;
}

////////////////////////////////////////////////////////////

/*
 * Helper function for pid_alloc.
 */
static
void
inc_nextpid(void)
{
	KASSERT(lock_do_i_hold(pidlock));

	nextpid++;
	if (nextpid > PID_MAX) {
		nextpid = PID_MIN;
	}
}

/*
 * pid_alloc: allocate a process id
 */
int
pid_alloc(pid_t *retval)
{
	struct pidinfo *pi;
	pid_t pid;
	int count;

	/* lock the table */
	lock_acquire(pidlock);

	if (nprocs == MAX_USERPROCS) {
		lock_release(pidlock);
		return EAGAIN;
	}

	/*
	 * The above test guarantees that this loop terminates, unless
	 * our nprocs count is off. Even so, KASSERT we aren't looping
	 * forever.
	 */
	count = 0;
	while (pidinfo_ary[nextpid % MAX_USERPROCS] != NULL) {

		/* avoid various boundary cases by allowing extra loops */
		KASSERT(count < MAX_USERPROCS*2+5);
		count++;

		inc_nextpid();
	}

	pid = nextpid;

	pi = pidinfo_create(pid, curproc->pid);
	if (pi==NULL) {
		lock_release(pidlock);
		return ENOMEM;
	}

	pi_put(pid, pi);

	inc_nextpid();

	lock_release(pidlock);

	*retval = pid;
	return 0;
}

/*
 * pid_unalloc - unallocate a process id (allocated with pid_alloc) that
 * hasn't run yet.
 */
void
pid_unalloc(pid_t theirpid)
{
	struct pidinfo *them;

	KASSERT(theirpid >= PID_MIN && theirpid <= PID_MAX);

	lock_acquire(pidlock);

	them = pi_get(theirpid);
	KASSERT(them != NULL);
	KASSERT(them->pi_exited == 0);
	KASSERT(them->pi_ppid == curproc->pid);

	/* keep pidinfo_destroy from complaining */
	them->pi_exitstatus = 0xdead;
	them->pi_exited = 1;
	them->pi_ppid = INVALID_PID;

	pi_drop(theirpid);

	lock_release(pidlock);
}

/*
 * pid_disown - disown any interest in waiting for a child's exit
 * status.
 */
void
pid_disown(pid_t theirpid)
{
	struct pidinfo *them;

	KASSERT(theirpid >= PID_MIN && theirpid <= PID_MAX);

	lock_acquire(pidlock);

	them = pi_get(theirpid);
	KASSERT(them != NULL);
	KASSERT(them->pi_ppid == curproc->pid);

	them->pi_ppid = INVALID_PID;
	if (them->pi_exited) {
		pi_drop(them->pi_pid);
	}

	lock_release(pidlock);
}

/*
 * pid_setexitstatus: Sets the exit status of the just exited thread with id PID. Must only
 * be called if the thread actually had a pid assigned. Wakes up any
 * parent thread waiters and disposes of the piddata if nobody else is still using it.
 */
void pid_setexitstatus(pid_t pid, int status) {
	struct pidinfo *pid_i;
	struct pidinfo *ppid_i;

	lock_acquire(pidlock);

	/* First, disown all children */
	for (int i=0; i<MAX_USERPROCS; i++) {
		if (pidinfo_ary[i] == NULL) {
			continue;
		}
		if (pidinfo_ary[i]->pi_ppid == pid) { // parent exited, disown children
			pidinfo_ary[i]->pi_ppid = INVALID_PID;
			if (pidinfo_ary[i]->pi_exited) { // child also exited, clean up pid info
				pi_drop(pidinfo_ary[i]->pi_pid);
			}
		}
	}


	pid_i = pi_get(pid);
	if (pid_i == NULL) {
		 return;
	}
	ppid_i = pi_get(pid_i->pi_ppid);
	if (!ppid_i) {
		return;
	}

	pid_i->pi_exitstatus = status;
	pid_i->pi_exited = 1;
	/* wake up our parent, if they're waiting on us */
  cv_broadcast(pid_i->pi_cv, pidlock);

	if (pid_i->pi_ppid == INVALID_PID) {
		/* no parent user process, drop the pidinfo */
		pi_drop(pid);
	}

	lock_release(pidlock);
}

/*
 * Waits on a pid, returning the exit status when it's available.
 * status and ret are kernel pointers, but pid/flags may come from
 * userland and may thus be maliciously invalid.
 *
 * status may be null, in which case the status is thrown away.
 */
int pid_wait_sleep(pid_t childpid, int *status) {
	KASSERT_CAN_SLEEP();
	struct pidinfo *child_info;

	/* Don't let a process wait for itself. */
	if (childpid == curproc->pid) {
		return EINVAL;
	}

	/*
	 * We don't support the Unix meanings of negative pids or 0
	 * (0 is INVALID_PID) and other code may break on them, so
	 * check now.
	 */
	if (childpid == INVALID_PID || childpid < 0) {
		return EINVAL;
	}

	lock_acquire(pidlock);

	child_info = pi_get(childpid);
	if (child_info == NULL) {
		lock_release(pidlock);
		DEBUG(DB_SYSCALL, "pid_wait_sleep invalid child PID given: %d\n", (int)childpid);
		return ESRCH;
	}

	KASSERT(child_info->pi_pid == childpid);

	/* Only allow waiting for own children. */
	if (child_info->pi_ppid != curproc->pid) {
		lock_release(pidlock);
		DEBUG(DB_SYSCALL, "waitpid called with non-child PID (parent: %d, child: %d)\n", curproc->pid, child_info->pi_pid);
		return EPERM;
	}

	/* NOTE: don't need to loop on this */
	if (child_info->pi_exited == 0) {
		cv_wait(child_info->pi_cv, pidlock); // blocks this thread (rescheduled) until the process exits
		KASSERT(child_info->pi_exited == 1);
	}

	if (status != NULL) {
		*status = child_info->pi_exitstatus;
	}

	child_info->pi_ppid = 0;
	pi_drop(child_info->pi_pid);

	lock_release(pidlock);
	return 0;
}

bool is_valid_pid(pid_t pid) {
	return pid >= PID_MIN && pid <= PID_MAX;
}
