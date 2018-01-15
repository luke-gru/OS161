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
 * Synchronization primitives.
 * The specifications of the functions are in synch.h.
 */

#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <wchan.h>
#include <thread.h>
#include <current.h>
#include <synch.h>

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *name, unsigned initial_count)
{
	struct semaphore *sem;

	sem = kmalloc(sizeof(*sem));
	if (sem == NULL) {
		return NULL;
	}

	sem->sem_name = kstrdup(name);
	if (sem->sem_name == NULL) {
		kfree(sem);
		return NULL;
	}

	sem->sem_wchan = wchan_create(sem->sem_name);
	if (sem->sem_wchan == NULL) {
		kfree(sem->sem_name);
		kfree(sem);
		return NULL;
	}

	spinlock_init(&sem->sem_lock);
	sem->sem_count = initial_count;

	return sem;
}

void
sem_destroy(struct semaphore *sem)
{
	KASSERT(sem != NULL);

	/* wchan_cleanup will assert if anyone's waiting on it */
	spinlock_cleanup(&sem->sem_lock);
	wchan_destroy(sem->sem_wchan);
	kfree(sem->sem_name);
	kfree(sem);
}

void
P(struct semaphore *sem)
{
	KASSERT(sem != NULL);

	/*
	 * May not block in an interrupt handler.
	 *
	 * For robustness, always check, even if we can actually
	 * complete the P without blocking.
	 */
	KASSERT(curthread->t_in_interrupt == false);

	/* Use the semaphore spinlock to protect the wchan as well. */
	spinlock_acquire(&sem->sem_lock);
	while (sem->sem_count == 0) {
		/*
		 *
		 * Note that we don't maintain strict FIFO ordering of
		 * threads going through the semaphore; that is, we
		 * might "get" it on the first try even if other
		 * threads are waiting. Apparently according to some
		 * textbooks semaphores must for some reason have
		 * strict ordering. Too bad. :-)
		 *
		 * Exercise: how would you implement strict FIFO
		 * ordering?
		 */
		wchan_sleep(sem->sem_wchan, &sem->sem_lock);
	}
	KASSERT(sem->sem_count > 0);
	sem->sem_count--;
	spinlock_release(&sem->sem_lock);
}

void
V(struct semaphore *sem)
{
	KASSERT(sem != NULL);

	spinlock_acquire(&sem->sem_lock);

	sem->sem_count++;
	KASSERT(sem->sem_count > 0);
	wchan_wakeone(sem->sem_wchan, &sem->sem_lock);

	spinlock_release(&sem->sem_lock);
}

////////////////////////////////////////////////////////////
//
// Lock.

struct lock *
lock_create(const char *name)
{
	struct lock *lock;

	lock = kmalloc(sizeof(*lock));
	if (lock == NULL) {
		return NULL;
	}

	lock->lk_name = kstrdup(name);
	if (lock->lk_name == NULL) {
		kfree(lock);
		return NULL;
	}

	HANGMAN_LOCKABLEINIT(&lock->lk_hangman, lock->lk_name);

	// add stuff here as needed
	lock->lk_wchan = wchan_create(lock->lk_name);
	if (lock->lk_wchan == NULL) {
		kfree(lock->lk_name);
		kfree(lock);
		return NULL;
	}
	spinlock_init(&lock->lk_lock);
	lock->holder = NULL;
	lock->locked = false;
	return lock;
}

void
lock_destroy(struct lock *lock)
{
	KASSERT(lock != NULL);
	if (lock->locked) {
		panic("attempt to destroy locked lock: %s\n", lock->lk_name);
	}

	// add stuff here as needed

	kfree(lock->lk_name);
	lock->holder = NULL;
	lock->locked = false;
	spinlock_cleanup(&lock->lk_lock);
	wchan_destroy(lock->lk_wchan);
	kfree(lock);
}

void
lock_acquire(struct lock *lock)
{
	KASSERT(lock != NULL);
	//KASSERT(curthread->t_in_interrupt == false); // TODO: use splhigh() to ensure no interrupts

	spinlock_acquire(&lock->lk_lock);
	/* Call this (atomically) before waiting for a lock */
	HANGMAN_WAIT(&curthread->t_hangman, &lock->lk_hangman);

	while (lock->locked) {
		wchan_sleep(lock->lk_wchan, &lock->lk_lock); // lock re-acquired when woken up
	}
	lock->locked = true;
	lock->holder = curthread;
	/* Call this (atomically) once the lock is acquired */
	HANGMAN_ACQUIRE(&curthread->t_hangman, &lock->lk_hangman);
	spinlock_release(&lock->lk_lock);
}

void
lock_release(struct lock *lock)
{
	KASSERT(lock != NULL);
	spinlock_acquire(&lock->lk_lock);
	if (lock_do_i_hold(lock)) {
		lock->locked = false;
		lock->holder = NULL;
		wchan_wakeone(lock->lk_wchan, &lock->lk_lock);
	}
	/* Call this (atomically) when the lock is released */
	HANGMAN_RELEASE(&curthread->t_hangman, &lock->lk_hangman);
	spinlock_release(&lock->lk_lock);
}

bool
lock_do_i_hold(struct lock *lock)
{
	KASSERT(lock != NULL);
	return lock->locked && curthread != NULL && lock->holder == curthread;
}

////////////////////////////////////////////////////////////
//
// CV


struct cv *
cv_create(const char *name)
{
	struct cv *cv;

	cv = kmalloc(sizeof(*cv));
	if (cv == NULL) {
		return NULL;
	}

	cv->cv_name = kstrdup(name);
	if (cv->cv_name==NULL) {
		kfree(cv);
		return NULL;
	}

	cv->cv_wchan = wchan_create(cv->cv_name);
	if (cv->cv_wchan == NULL) {
		kfree(cv->cv_name);
		kfree(cv);
		return NULL;
	}
	spinlock_init(&cv->cv_wchan_lk);

	return cv;
}

void
cv_destroy(struct cv *cv)
{
	KASSERT(cv != NULL);
	spinlock_cleanup(&cv->cv_wchan_lk);
	wchan_destroy(cv->cv_wchan);

	kfree(cv->cv_name);
	kfree(cv);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	KASSERT(cv != NULL);
	KASSERT(lock != NULL);
	if (lock_do_i_hold(lock)) {
		lock_release(lock);
		spinlock_acquire(&cv->cv_wchan_lk);
		wchan_sleep(cv->cv_wchan, &cv->cv_wchan_lk); // blocking
		spinlock_release(&cv->cv_wchan_lk);
		lock_acquire(lock);
	}
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	KASSERT(cv != NULL);
	KASSERT(lock != NULL);
	if (lock_do_i_hold(lock)) {
		spinlock_acquire(&cv->cv_wchan_lk);
		wchan_wakeone(cv->cv_wchan, &cv->cv_wchan_lk);
		spinlock_release(&cv->cv_wchan_lk);
	}
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	KASSERT(cv != NULL);
	KASSERT(lock != NULL);
	if (lock_do_i_hold(lock)) {
		spinlock_acquire(&cv->cv_wchan_lk);
		wchan_wakeall(cv->cv_wchan, &cv->cv_wchan_lk);
		spinlock_release(&cv->cv_wchan_lk);
	}
}

struct rwlock *rwlock_create(const char *name) {
	KASSERT(name != NULL);
	struct rwlock *rw;
	rw = kmalloc(sizeof(struct rwlock));
	if (rw == NULL) {
		return NULL;
	}
	rw->rwlock_name = kstrdup(name);
	if (rw->rwlock_name == NULL) {
		kfree(rw); return NULL;
	}
	rw->rw_wchan = wchan_create(rw->rwlock_name);
	spinlock_init(&rw->rw_wchan_lk);
	rw->lock = lock_create(rw->rwlock_name);
	rw->reader_count = 0;
	rw->writer_count = 0;
	rw->write_requests = 0;
	return rw;
}

void rwlock_destroy(struct rwlock *rw) {
	KASSERT(rw != NULL);
	wchan_destroy(rw->rw_wchan);
	spinlock_cleanup(&rw->rw_wchan_lk);
	lock_destroy(rw->lock);
	rw->reader_count = 0;
	rw->writer_count = 0;
	rw->write_requests = 0;
	kfree(rw->rwlock_name);
	kfree(rw);
}

void rwlock_acquire_read(struct rwlock *rw) {
	lock_acquire(rw->lock);
	while (rw->writer_count > 0 || rw->write_requests > 0) {
		lock_release(rw->lock);
		spinlock_acquire(&rw->rw_wchan_lk);
		wchan_sleep(rw->rw_wchan, &rw->rw_wchan_lk);
		spinlock_release(&rw->rw_wchan_lk);
		lock_acquire(rw->lock);
	}
	rw->reader_count++;
	lock_release(rw->lock);
}

void rwlock_release_read(struct rwlock *rw) {
	lock_acquire(rw->lock);
	KASSERT(rw->reader_count > 0);
	rw->reader_count--;
	lock_release(rw->lock);
	if (rw->reader_count == 0) {
		spinlock_acquire(&rw->rw_wchan_lk);
		wchan_wakeall(rw->rw_wchan, &rw->rw_wchan_lk);
		spinlock_release(&rw->rw_wchan_lk);
	}
}

void rwlock_acquire_write(struct rwlock *rw) {
	lock_acquire(rw->lock);
	rw->write_requests++;
	while (rw->writer_count > 0 || rw->reader_count > 0) {
		lock_release(rw->lock);
		spinlock_acquire(&rw->rw_wchan_lk);
		wchan_sleep(rw->rw_wchan, &rw->rw_wchan_lk);
		spinlock_release(&rw->rw_wchan_lk);
		lock_acquire(rw->lock);
	}
	rw->write_requests--;
	rw->writer_count++;
	lock_release(rw->lock);
}

void rwlock_release_write(struct rwlock *rw) {
	lock_acquire(rw->lock);
	rw->writer_count--;
	lock_release(rw->lock);
	if (rw->writer_count == 0) {
		spinlock_acquire(&rw->rw_wchan_lk);
		wchan_wakeall(rw->rw_wchan, &rw->rw_wchan_lk);
		spinlock_release(&rw->rw_wchan_lk);
	}
}
