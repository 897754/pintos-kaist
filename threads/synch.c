/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* 세마포어 SEMA를 VALUE로 초기화합니다. 세마포어는
   두 가지 원자적 연산을 통해 조작되는 비음수 정수입니다:

   - down 또는 "P": 값이 양수가 될 때까지 기다린 후,
     값을 감소시킵니다.

   - up 또는 "V": 값을 증가시키고 (대기 중인 스레드가 있다면 하나를 깨웁니다). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

static bool
priority_ful (const struct list_elem *a_, const struct list_elem *b_,
            void *aux UNUSED) 
{
  const struct thread *a = list_entry (a_, struct thread, elem);
  const struct thread *b = list_entry (b_, struct thread, elem);
  
  return a->priority > b->priority;
}

/* 세마포어에 대한 Down 또는 "P" 연산입니다. SEMA의 값이
   양수가 될 때까지 기다린 후 원자적으로 값을 감소시킵니다.

   이 함수는 대기할 수 있으므로, 인터럽트 핸들러 내에서 호출해서는 안 됩니다.
   이 함수는 인터럽트를 비활성화한 상태에서 호출될 수 있지만, 대기하게 되면
   다음에 스케줄된 스레드가 인터럽트를 다시 활성화할 가능성이 큽니다.
   이 함수는 sema_down 함수입니다. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		// list_insert_ordered(&sema->waiters, &thread_current ()->elem, priority_ful, NULL);
		list_push_back(&sema->waiters, &thread_current ()->elem);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* 세마포어가 이미 0이 아닌 경우에만 세마포어에 대한 Down 또는 "P" 연산을 수행합니다.
   세마포어가 감소된 경우에는 true를 반환하고, 그렇지 않으면 false를 반환합니다.

   이 함수는 인터럽트 핸들러에서 호출될 수 있습니다. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* 세마포어에서 Up 또는 "V" 연산. SEMA의 값을 증가시키고, 대기 중인 스레드가 있다면 그 중 하나를 깨웁니다.
   이 함수는 인터럽트 핸들러에서 호출될 수 있습니다.*/
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (!list_empty (&sema->waiters)) {
		list_sort(&sema->waiters, priority_ful, NULL);
		thread_unblock (list_entry (list_pop_front (&sema->waiters), struct thread, elem));
	}
	sema->value++;

	thread_preempt ();
	intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	lock->old_priority = -1;
	sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	struct thread *curr = thread_current ();

	if (!thread_mlfqs) {
		curr->waiting_lock = lock;
		// lock holder가 있다면 
		if (lock->holder != NULL) {
			struct list *locklist = &lock->holder->lock_list;
			// 그리고 lock을 처음으로 요구한다면 lock의 old_priority를 lock holder의 old_priority로 변경
			// lock holder의 priority를 curr priority로 donate해줘야 하기 때문.
			if (lock->old_priority == -1) {
				lock->old_priority = lock->holder->priority;
				list_insert_ordered(locklist, &lock->lock_elem, lock_priority_ful, NULL);
			}
			// lock holder의 waiting_lock을 파악 (물고 물리는 경우 끝까지 확인 (while 사용))
			// waiting_lock holder 의 priority를 모두 curr priority로 donate.
			struct lock *wait_lock = lock->holder->waiting_lock;
			while (wait_lock) {
				wait_lock->holder->priority = curr->priority;
				wait_lock = wait_lock->holder->waiting_lock;
			}
			lock->holder->priority = curr->priority;
		}
	}

	sema_down (&lock->semaphore);
	
	lock->holder = curr;
	curr->waiting_lock = NULL;
}

static bool
lock_priority_ful (const struct list_elem *a_, const struct list_elem *b_,
            void *aux UNUSED) 
{
  const struct lock *a = list_entry (a_, struct lock, lock_elem);
  const struct lock *b = list_entry (b_, struct lock, lock_elem);
  
  return a->old_priority < b->old_priority;
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));
	
	struct thread *curr = thread_current ();

	if (!thread_mlfqs) {
		if (lock->old_priority != -1) { 
			if (list_next(&lock->lock_elem) != list_end(&curr->lock_list)) {
				list_entry(list_next(&lock->lock_elem), struct lock, lock_elem)->old_priority = lock->old_priority; 
			}
			else {
				lock->holder->priority = lock->old_priority;
			}
			lock->old_priority = -1;
			list_remove(&lock->lock_elem);
		}
	}
	lock->holder = NULL;

	sema_up (&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	//list_insert_ordered(&cond->waiters, &waiter.elem, sema_priority_ful, NULL);
	list_push_back(&cond->waiters, &waiter.elem);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

static bool
sema_priority_ful (const struct list_elem *a_, const struct list_elem *b_,
            void *aux UNUSED) 
{
  const struct semaphore_elem *a = list_entry (a_, struct semaphore_elem, elem);
  const struct semaphore_elem *b = list_entry (b_, struct semaphore_elem, elem);

  struct list *t1 = &(a->semaphore.waiters);
  struct list *t2 = &(b->semaphore.waiters);
  
  return list_entry (list_begin (t1), struct thread, elem)->priority < list_entry (list_begin (t2), struct thread, elem)->priority;
}


/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));


	if (!list_empty (&cond->waiters)) {
		struct list_elem *max = list_max (&cond->waiters, sema_priority_ful, NULL);
		list_remove(max);
		sema_up (&list_entry (max, struct semaphore_elem, elem)->semaphore);
	}


		// sema_up (&list_entry (list_pop_front (&cond->waiters), struct semaphore_elem, elem)->semaphore);
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
