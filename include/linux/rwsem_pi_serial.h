/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_RWSEM_PI_SERIAL_H
#define _LINUX_RWSEM_PI_SERIAL_H

#include <linux/bug.h>
#include <linux/lockdep.h>
#include <linux/rtmutex.h>

/*
 * Single-owner PI semaphore for sleepable task context. Both modes are
 * exclusive, including in lockdep. No simultaneous readers, recursion or
 * non-owner release. Requires CONFIG_RT_MUTEXES; not a native rwsem substitute.
 */
struct rwsem_pi_serial {
	struct rt_mutex lock;
	bool write; /* Logical mode, accessed only by the owner. */
};

#define init_rwsem_pi_serial(sem)	rt_mutex_init(&(sem)->lock)

static inline void rwsem_pi_serial_down_read(struct rwsem_pi_serial *sem)
{
	rt_mutex_lock(&sem->lock);
	sem->write = false;
}

static inline void rwsem_pi_serial_down_write(struct rwsem_pi_serial *sem)
{
	rt_mutex_lock(&sem->lock);
	sem->write = true;
}

static inline int rwsem_pi_serial_down_read_trylock(struct rwsem_pi_serial *sem)
{
	if (!rt_mutex_trylock(&sem->lock))
		return 0;
	sem->write = false;
	return 1;
}

static inline int rwsem_pi_serial_down_write_trylock(struct rwsem_pi_serial *sem)
{
	if (!rt_mutex_trylock(&sem->lock))
		return 0;
	sem->write = true;
	return 1;
}

static inline int rwsem_pi_serial_down_read_interruptible(struct rwsem_pi_serial *sem)
{
	int ret = rt_mutex_lock_interruptible(&sem->lock);

	if (!ret)
		sem->write = false;
	return ret;
}

static inline int rwsem_pi_serial_down_read_killable(struct rwsem_pi_serial *sem)
{
	int ret = rt_mutex_lock_killable(&sem->lock);

	if (!ret)
		sem->write = false;
	return ret;
}

static inline int rwsem_pi_serial_down_write_killable(struct rwsem_pi_serial *sem)
{
	int ret = rt_mutex_lock_killable(&sem->lock);

	if (!ret)
		sem->write = true;
	return ret;
}

static inline void rwsem_pi_serial_up_read(struct rwsem_pi_serial *sem)
{
	WARN_ON_ONCE(sem->write);
	rt_mutex_unlock(&sem->lock);
}

static inline void rwsem_pi_serial_up_write(struct rwsem_pi_serial *sem)
{
	WARN_ON_ONCE(!sem->write);
	rt_mutex_unlock(&sem->lock);
}

/* No physical unlock, lockdep transition, reader admission or PI deboost. */
static inline void rwsem_pi_serial_downgrade_write(struct rwsem_pi_serial *sem)
{
	lockdep_assert_held(&sem->lock);
	WARN_ON_ONCE(!sem->write);
	sem->write = false;
}

#endif /* _LINUX_RWSEM_PI_SERIAL_H */
