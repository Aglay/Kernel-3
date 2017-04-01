/*
 * semaphore.c
 *
 *  Created on: 04.09.2015
 *      Author: pascal
 */

#include "semaphore.h"
#include "thread.h"
#include "scheduler.h"

void semaphore_init(semaphore_t *sem, int64_t count)
{
	if(sem == NULL)
		return;

	sem->lock = LOCK_UNLOCKED;
	sem->count = count;
	sem->waiting = queue_create();
}

void semaphore_destroy(semaphore_t *sem)
{
	if(sem == NULL)
		return;

	lock(&sem->lock);
	queue_destroy(sem->waiting);
}

void semaphore_acquire(semaphore_t *sem)
{
	if(sem == NULL)
		return;

retry:
	if(__sync_fetch_and_add(&sem->count, -1) <= 0)
	{
		lock(&sem->lock);
		queue_enqueue(sem->waiting, currentThread);
		unlock(&sem->lock);
		if(!thread_block_self((thread_bail_out_t)semaphore_release, sem)) goto retry;
		yield();
	}
}

void semaphore_release(semaphore_t *sem)
{
	if(sem == NULL)
		return;

	__sync_add_and_fetch(&sem->count, 1);
	if(queue_size(sem->waiting) > 0)
	{
		lock(&sem->lock);
		thread_unblock(queue_dequeue(sem->waiting));
		unlock(&sem->lock);
	}
}
