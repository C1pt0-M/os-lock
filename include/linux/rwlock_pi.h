/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RWLOCK_PI_H
#define _LINUX_RWLOCK_PI_H

#include <linux/spinlock.h>

#ifdef CONFIG_RWLOCK_PI
#include <linux/rtmutex.h>

/* Task context only. No recursive readers, upgrades or CPU pinning. */
typedef struct {
	atomic_t readers;
	struct rt_mutex gate;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map dep_map;
#endif
} rwlock_pi_t;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
#define __RWLOCK_PI_MAP(lockname) \
	.dep_map = { .name = #lockname, .wait_type_inner = LD_WAIT_SLEEP },
#else
#define __RWLOCK_PI_MAP(lockname)
#endif

#define __RWLOCK_PI_UNLOCKED(lockname) { \
	.readers = ATOMIC_INIT(-2147483647 - 1), \
	.gate = __RT_MUTEX_INITIALIZER_TAG(lockname.gate, RT_MUTEX_PI_TAG), \
	__RWLOCK_PI_MAP(lockname) \
}
#define DEFINE_RWLOCK_PI(name) rwlock_pi_t name = __RWLOCK_PI_UNLOCKED(name)

void __rwlock_init_pi(rwlock_pi_t *lock, const char *name,
		      struct lock_class_key *key);
#define rwlock_init_pi(lock) do { \
	static struct lock_class_key __key; \
	__rwlock_init_pi((lock), #lock, &__key); \
} while (0)

void pi_read_lock(rwlock_pi_t *lock);
void pi_write_lock(rwlock_pi_t *lock);
int pi_read_trylock(rwlock_pi_t *lock);
int pi_write_trylock(rwlock_pi_t *lock);
void pi_read_unlock(rwlock_pi_t *lock);
void pi_write_unlock(rwlock_pi_t *lock);
#else
typedef rwlock_t rwlock_pi_t;
#define __RWLOCK_PI_UNLOCKED(name) __RW_LOCK_UNLOCKED(name)
#define DEFINE_RWLOCK_PI(name) DEFINE_RWLOCK(name)
#define rwlock_init_pi(lock) rwlock_init(lock)
#define pi_read_lock(lock) read_lock(lock)
#define pi_write_lock(lock) write_lock(lock)
#define pi_read_trylock(lock) read_trylock(lock)
#define pi_write_trylock(lock) write_trylock(lock)
#define pi_read_unlock(lock) read_unlock(lock)
#define pi_write_unlock(lock) write_unlock(lock)
#endif
#endif
