/*
 * Copyright (c) 2001, 2002, 2009
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
 * Driver code is in kern/tests/synchprobs.c We will replace that file. This
 * file is yours to modify as you see fit.
 *
 * You should implement your solution to the stoplight problem below. The
 * quadrant and direction mappings for reference: (although the problem is, of
 * course, stable under rotation)
 *
 *   |0 |
 * -     --
 *    01  1
 * 3  32
 * --    --
 *   | 2|
 *
 * As way to think about it, assuming cars drive on the right: a car entering
 * the intersection from direction X will enter intersection quadrant X first.
 * The semantics of the problem are that once a car enters any quadrant it has
 * to be somewhere in the intersection until it call leaveIntersection(),
 * which it should call while in the final quadrant.
 *
 * As an example, let's say a car approaches the intersection and needs to
 * pass through quadrants 0, 3 and 2. Once you call inQuadrant(0), the car is
 * considered in quadrant 0 until you call inQuadrant(3). After you call
 * inQuadrant(2), the car is considered in quadrant 2 until you call
 * leaveIntersection().
 *
 * You will probably want to write some helper functions to assist with the
 * mappings. Modular arithmetic can help, e.g. a car passing straight through
 * the intersection entering from direction X will leave to direction (X + 2)
 * % 4 and pass through quadrants X and (X + 3) % 4.  Boo-yah.
 *
 * Your solutions below should call the inQuadrant() and leaveIntersection()
 * functions in synchprobs.c to record their progress.
 */

#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>
#include <current.h>

/*
 * Called by the driver during initialization.
 */

 struct semaphore *quad0, *quad1, *quad2, *quad3, *intersection;
 struct lock *lk;


void
stoplight_init() {
	quad0 = sem_create("Quadrant 0", 1); // lock for quadrants
	quad1 = sem_create("Quadrant 1", 1);
	quad2 = sem_create("Quadrant 2", 1);
	quad3 = sem_create("Quadrant 3", 1);
  // only 3 cars may be in the intersection at once to prevent deadlock
  intersection = sem_create("Intersection", 3);
}

/*
 * Called by the driver during teardown.
 */

void stoplight_cleanup() {
	return;
}

const char *sem_name(struct semaphore *);
const char *sem_name(struct semaphore *sem) {
  if (quad0 == sem) {
    return "quad 0";
  } else if (quad1 == sem) {
    return "quad 1";
  } else if (quad2 == sem) {
    return "quad 2";
  } else if (quad3 == sem) {
    return "quad 3";
  } else if (intersection == sem) {
    return "intersection";
  } else {
    panic("bad semaphore");
    return "??";
  }
}

void My_P(struct semaphore *);
void My_P(struct semaphore *sem) {
  kprintf_t("%s waiting on sem %s\n", curthread->t_name, sem_name(sem));
  P(sem);
}
void My_V(struct semaphore *);
void My_V(struct semaphore *sem) {
  kprintf_t("%s signaling sem %s\n", curthread->t_name, sem_name(sem));
  V(sem);
}

struct semaphore *get_sem_for_quad(int);
struct semaphore *get_sem_for_quad(int quad) {
  switch (quad) {
    case 0:
      return quad0;
    case 1:
      return quad1;
    case 2:
      return quad2;
    case 3:
      return quad3;
    default:
      panic("invalid quadrant: %d", quad);
      return quad0;
  }
}

void
turnright(uint32_t direction, uint32_t index)
{
  struct semaphore *curSem;
  int nextQuad = (int)direction;
  curSem = get_sem_for_quad(nextQuad);
  P(intersection);
  My_P(curSem);
  inQuadrant(nextQuad, index);

  leaveIntersection(index);

  My_V(curSem);
  V(intersection);
}

void
gostraight(uint32_t direction, uint32_t index)
{
  struct semaphore *sem1, *sem2;
  int nextQuad = (int)direction;
  sem1 = get_sem_for_quad(nextQuad);

  P(intersection);
  My_P(sem1);
  inQuadrant(nextQuad, index);

  nextQuad = ((int)direction + 3) % 4;
  sem2 = get_sem_for_quad(nextQuad);
  My_P(sem2);
  inQuadrant(nextQuad, index);
  My_V(sem1);

  leaveIntersection(index);
  My_V(sem2);
  V(intersection);
}

void
turnleft(uint32_t direction, uint32_t index)
{
  struct semaphore *sem1, *sem2, *sem3;
  int nextQuad = (int)direction;
  sem1 = get_sem_for_quad(nextQuad);

  P(intersection);
  My_P(sem1);
  inQuadrant(nextQuad, index);

  nextQuad = ((int)direction + 3) % 4;
  sem2 = get_sem_for_quad(nextQuad);
  My_P(sem2);
  inQuadrant(nextQuad, index);
  My_V(sem1);

  nextQuad = ((int)direction + 2) % 4;
  sem3 = get_sem_for_quad(nextQuad);
  My_P(sem3);
  inQuadrant(nextQuad, index);
  My_V(sem2);

  leaveIntersection(index);
  My_V(sem3);
  V(intersection);
}
