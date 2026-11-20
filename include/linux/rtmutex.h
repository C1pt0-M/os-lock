/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RT Mutexes: blocking mutual exclusion locks with PI support
 *
 * started by Ingo Molnar and Thomas Gleixner:
 *
 *  Copyright (C) 2004-2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *  Copyright (C) 2006, Timesys Corp., Thomas Gleixner <tglx@timesys.com>
 *
 * This file contains the public data structure and API definitions.
 */

#ifndef __LINUX_RT_MUTEX_H
#define __LINUX_RT_MUTEX_H

#include <linux/linkage.h>
#include <linux/rbtree.h>
#include <linux/spinlock_types.h>

extern int max_lock_depth; /* for sysctl */

/**
 * The rt_mutex structure
 *
 * @wait_lock:	spinlock to protect the structure
 * @waiters:	rbtree root to enqueue waiters in priority order;
 *              caches top-waiter (leftmost node).
 * @owner:	the mutex owner
 */
#if defined(CONFIG_MUTEX_PI) || defined(CONFIG_RWSEM_PI)
/* Immutable for a published lock; does not mean the lock is held. */
#define RT_MUTEX_PI_TAG		8UL
#else
#define RT_MUTEX_PI_TAG		0UL
#endif

struct rt_mutex {
#if RT_MUTEX_PI_TAG
	/* Shares the owner word with the enclosing mutex or rwsem. */
	struct task_struct	*owner;
#endif
	raw_spinlock_t		wait_lock;
	struct rb_root_cached   waiters;
#if !RT_MUTEX_PI_TAG
	struct task_struct	*owner;
#endif
#ifdef CONFIG_DEBUG_RT_MUTEXES
	int			save_state;
	const char		*name, *file;
	int			line;
	void			*magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};

struct rt_mutex_waiter;
struct hrtimer_sleeper;

#ifdef CONFIG_DEBUG_RT_MUTEXES
 extern int rt_mutex_debug_check_no_locks_freed(const void *from,
						unsigned long len);
 extern void rt_mutex_debug_check_no_locks_held(struct task_struct *task);
#else
 static inline int rt_mutex_debug_check_no_locks_freed(const void *from,
						       unsigned long len)
 {
	return 0;
 }
# define rt_mutex_debug_check_no_locks_held(task)	do { } while (0)
#endif

#ifdef CONFIG_DEBUG_RT_MUTEXES
# define __DEBUG_RT_MUTEX_INITIALIZER(mutexname) \
	, .name = #mutexname, .file = __FILE__, .line = __LINE__

# define rt_mutex_init(mutex) \
do { \
	static struct lock_class_key __key; \
	__rt_mutex_init(mutex, __func__, &__key); \
} while (0)

 extern void rt_mutex_debug_task_free(struct task_struct *tsk);
#else
# define __DEBUG_RT_MUTEX_INITIALIZER(mutexname)
# define rt_mutex_init(mutex)			__rt_mutex_init(mutex, NULL, NULL)
# define rt_mutex_debug_task_free(t)			do { } while (0)
#endif

#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define __DEP_MAP_RT_MUTEX_INITIALIZER(mutexname) \
	, .dep_map = { .name = #mutexname }
#else
#define __DEP_MAP_RT_MUTEX_INITIALIZER(mutexname)
#endif

#define __RT_MUTEX_INITIALIZER_TAG(mutexname, tag) \
	{ .wait_lock = __RAW_SPIN_LOCK_UNLOCKED(mutexname.wait_lock) \
	, .waiters = RB_ROOT_CACHED \
	, .owner = (struct task_struct *)(tag) \
	__DEBUG_RT_MUTEX_INITIALIZER(mutexname) \
	__DEP_MAP_RT_MUTEX_INITIALIZER(mutexname)}

#define __RT_MUTEX_INITIALIZER(mutexname) \
	__RT_MUTEX_INITIALIZER_TAG(mutexname, 0)

#define DEFINE_RT_MUTEX(mutexname) \
	struct rt_mutex mutexname = __RT_MUTEX_INITIALIZER(mutexname)

/**
 * rt_mutex_is_locked - is the mutex locked
 * @lock: the mutex to be queried
 *
 * Returns 1 if the mutex is locked, 0 if unlocked.
 */
static inline int rt_mutex_is_locked(struct rt_mutex *lock)
{
	return ((unsigned long)READ_ONCE(lock->owner) & ~RT_MUTEX_PI_TAG) != 0;
}

extern void __rt_mutex_init(struct rt_mutex *lock, const char *name, struct lock_class_key *key);
extern void rt_mutex_destroy(struct rt_mutex *lock);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
extern void rt_mutex_lock_nested(struct rt_mutex *lock, unsigned int subclass);
#define rt_mutex_lock(lock) rt_mutex_lock_nested(lock, 0)
#else
extern void rt_mutex_lock(struct rt_mutex *lock);
#define rt_mutex_lock_nested(lock, subclass) rt_mutex_lock(lock)
#endif

extern int rt_mutex_lock_interruptible(struct rt_mutex *lock);
extern int rt_mutex_timed_lock(struct rt_mutex *lock,
			       struct hrtimer_sleeper *timeout);

extern int rt_mutex_trylock(struct rt_mutex *lock);

extern void rt_mutex_unlock(struct rt_mutex *lock);

#endif
