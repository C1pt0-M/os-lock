// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/rwlock_pi.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include "rtmutex_common.h"
#include "rwbase_pi.h"

static bool pi_invalid_context(void)
{
	return in_interrupt() || in_atomic() || irqs_disabled() ||
	       READ_ONCE(current->state) != TASK_RUNNING;
}

static void pi_require_context(void)
{
	if (WARN_ON_ONCE(pi_invalid_context()))
		BUG();
}

static void pi_require_not_held(rwlock_pi_t *lock)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	if (debug_locks && !current->lockdep_recursion &&
	    WARN_ON_ONCE(lockdep_is_held(lock)))
		BUG();
#endif
}

void __rwlock_init_pi(rwlock_pi_t *lock, const char *name,
		      struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	debug_check_no_locks_freed(lock, sizeof(*lock));
	lockdep_init_map_wait(&lock->dep_map, name, key, 0, LD_WAIT_SLEEP);
#endif
	atomic_set(&lock->readers, RWBASE_PI_READER_BIAS);
	__rt_mutex_init(&lock->gate, name, key);
	lock->gate.owner = (struct task_struct *)RT_MUTEX_PI_TAG;
}
EXPORT_SYMBOL(__rwlock_init_pi);

void pi_read_lock(rwlock_pi_t *lock)
{
	pi_require_context();
	pi_require_not_held(lock);
	might_sleep();
	lock_acquire_shared(&lock->dep_map, 0, 0, NULL, _RET_IP_);
	rwbase_pi_read_lock(&lock->readers, &lock->gate, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(pi_read_lock);

void pi_write_lock(rwlock_pi_t *lock)
{
	pi_require_context();
	pi_require_not_held(lock);
	might_sleep();
	lock_acquire_exclusive(&lock->dep_map, 0, 0, NULL, _RET_IP_);
	rwbase_pi_write_lock(&lock->readers, &lock->gate, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(pi_write_lock);

int pi_read_trylock(rwlock_pi_t *lock)
{
	if (WARN_ON_ONCE(pi_invalid_context()))
		return 0;
	pi_require_not_held(lock);
	if (!rwbase_pi_read_trylock(&lock->readers, &lock->gate))
		return 0;
	lock_acquire_shared(&lock->dep_map, 0, 1, NULL, _RET_IP_);
	return 1;
}
EXPORT_SYMBOL(pi_read_trylock);

int pi_write_trylock(rwlock_pi_t *lock)
{
	if (WARN_ON_ONCE(pi_invalid_context()))
		return 0;
	pi_require_not_held(lock);
	if (!rwbase_pi_write_trylock(&lock->readers, &lock->gate))
		return 0;
	lock_acquire_exclusive(&lock->dep_map, 0, 1, NULL, _RET_IP_);
	return 1;
}
EXPORT_SYMBOL(pi_write_trylock);

void pi_read_unlock(rwlock_pi_t *lock)
{
	pi_require_context();
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	lockdep_assert_held_read(lock);
#endif
	lock_release(&lock->dep_map, _RET_IP_);
	rwbase_pi_read_unlock(&lock->readers, &lock->gate);
}
EXPORT_SYMBOL(pi_read_unlock);

void pi_write_unlock(rwlock_pi_t *lock)
{
	pi_require_context();
	if (WARN_ON_ONCE(rt_mutex_owner(&lock->gate) != current))
		BUG();
	lock_release(&lock->dep_map, _RET_IP_);
	rwbase_pi_write_release(&lock->readers, &lock->gate, false);
}
EXPORT_SYMBOL(pi_write_unlock);
