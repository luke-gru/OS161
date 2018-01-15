/*
 * All the contents of this file are overwritten during automated
 * testing. Please consider this before changing anything in this file.
 */

#include <types.h>
#include <lib.h>
#include <clock.h>
#include <thread.h>
#include <synch.h>
#include <test.h>
#include <kern/test161.h>
#include <spinlock.h>

/*
 * Use these stubs to test your reader-writer locks.
 */
 struct spinlock status_lock;
 static bool test_status = TEST161_FAIL;

 static
 bool
 failif(bool condition, const char *msg) {
 	if (condition) {
 		spinlock_acquire(&status_lock);
 		test_status = TEST161_FAIL;
		kprintf_n(msg);
 		spinlock_release(&status_lock);
 	}
 	return condition;
 }

 #define READ_THREADS 10
 #define WRITE_THREADS 10
 volatile int max_reads = 0;
 volatile int max_writes = 0;
 volatile int num_reads = 0;
 volatile int num_writes = 0;
 volatile int shared_val = -1;
 struct rwlock *lk;
 struct semaphore *thread_done_sem;
 struct lock *mutex;

 static void rwtest_read(void *junk, unsigned long i) {
	 (void)junk; (void)i;
	 rwlock_acquire_read(lk);
	 random_yielder(4);

	 lock_acquire(mutex);
	 num_reads++;
	 if (lk->reader_count > max_reads) {
		 max_reads = lk->reader_count;
	 }
	 random_yielder(4);
	 lock_release(mutex);

	 rwlock_release_read(lk);
	 random_yielder(4);
	 kprintf_n("thread %d done (read)\n", (int)i);
	 V(thread_done_sem);
 }

 static void rwtest_write(void *junk, unsigned long i) {
	(void)junk;
	rwlock_acquire_write(lk);
	random_yielder(4);
	num_writes++;
	max_writes++;
	random_yielder(4);
	shared_val = (int)i;
	if (max_writes > 1) {
		failif(max_writes > 1, "invalid max writes");
		V(thread_done_sem);
		return;
	}
	max_writes--;
	random_yielder(4);
	rwlock_release_write(lk);
	kprintf_n("thread %d done (write)\n", (int)i);
	V(thread_done_sem);
 }

int rwtest(int nargs, char **args) {
	(void)nargs;
	(void)args;

	spinlock_init(&status_lock);
	test_status = TEST161_SUCCESS;
	kprintf_n("starting rwt1\n");
	thread_done_sem = sem_create("Threads done sem", 0);

	lk = rwlock_create("rwtest lock");
	mutex = lock_create("mutex");
	int result;
	for (int i = 0; i < READ_THREADS + WRITE_THREADS; i++) {
		if (i % 2 == 0) {
			result = thread_fork("rwtest (read)", NULL, rwtest_read, NULL, i);
			if (result) {
				panic("rwtest: thread_fork failed: %s\n", strerror(result));
			}
		} else {
			result = thread_fork("rwtest (write)", NULL, rwtest_write, NULL, i);
			if (result) {
				panic("rwtest: thread_fork failed: %s\n", strerror(result));
			}
		}
	}

	for (int i = 0; i < READ_THREADS + WRITE_THREADS; i++) {
		P(thread_done_sem);
		kprintf_n("done waiting for thread %d\n", i);
	}

	failif(max_reads <= 1, "max_reads <= 1\n");
	failif(max_writes != 0, "max_writes != 0\n");
	failif(num_reads != READ_THREADS, "num_reads wrong\n");
	failif(num_writes != WRITE_THREADS, "num_writes wrong\n");
	failif(shared_val < 0, "shared_val wrong");

	kprintf_n("max reads: %d\n", max_reads);
	kprintf_n("shared val: %d\n", shared_val);

	success(test_status, SECRET, "rwt1");

	sem_destroy(thread_done_sem); thread_done_sem = NULL;
	rwlock_destroy(lk); lk = NULL;
	lock_destroy(mutex); mutex = NULL;
	max_reads = 0; max_writes = 0;
	num_reads = 0; num_writes = 0;
	shared_val = -1;

	return 0;
}

int rwtest2(int nargs, char **args) {
	(void)nargs;
	(void)args;

	kprintf_n("rwt2 unimplemented\n");
	success(TEST161_FAIL, SECRET, "rwt2");

	return 0;
}

int rwtest3(int nargs, char **args) {
	(void)nargs;
	(void)args;

	kprintf_n("rwt3 unimplemented\n");
	success(TEST161_FAIL, SECRET, "rwt3");

	return 0;
}

int rwtest4(int nargs, char **args) {
	(void)nargs;
	(void)args;

	kprintf_n("rwt4 unimplemented\n");
	success(TEST161_FAIL, SECRET, "rwt4");

	return 0;
}

int rwtest5(int nargs, char **args) {
	(void)nargs;
	(void)args;

	kprintf_n("rwt5 unimplemented\n");
	success(TEST161_FAIL, SECRET, "rwt5");

	return 0;
}
