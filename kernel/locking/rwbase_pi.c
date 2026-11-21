// SPDX-License-Identifier: GPL-2.0
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/sched/wake_q.h>
#include "rtmutex_common.h"
#include "rwbase_pi.h"

bool rwbase_pi_read_trylock(atomic_t *readers, struct rt_mutex *gate)
{
	int count = atomic_read(readers);

	while (count < 0) {
		if (likely(atomic_try_cmpxchg_acquire(readers, &count,
						     count + 1)))
			return true;
	}
	return false;
}

static noinline int __sched rwbase_pi_read_slowlock(atomic_t *readers, struct rt_mutex *gate,
						   int state)
{
	int ret;

	raw_spin_lock_irq(&gate->wait_lock);
	ret = rt_mutex_lock_locked_nolockdep(gate, state);
	if (!ret)
		atomic_inc(readers);
	raw_spin_unlock_irq(&gate->wait_lock);
	if (!ret)
		rt_mutex_unlock_nolockdep(gate);
	return ret;
}

int __sched rwbase_pi_read_lock(atomic_t *readers, struct rt_mutex *gate, int state)
{
	if (likely(rwbase_pi_read_trylock(readers, gate)))
		return 0;
	return rwbase_pi_read_slowlock(readers, gate, state);
}

static noinline void rwbase_pi_wake_writer(atomic_t *readers, struct rt_mutex *gate)
{
	unsigned long flags;
	DEFINE_WAKE_Q(wake_q);

	raw_spin_lock_irqsave(&gate->wait_lock, flags);
	/* A writer may have already finished the drain or rolled it back. */
	if (!atomic_read(readers))
		wake_q_add(&wake_q, rt_mutex_owner(gate));
	raw_spin_unlock_irqrestore(&gate->wait_lock, flags);
	wake_up_q(&wake_q);
}

void rwbase_pi_read_unlock(atomic_t *readers, struct rt_mutex *gate)
{
	/* Publish this reader's critical section, including the last exit. */
	if (unlikely(atomic_dec_and_test(readers)))
		rwbase_pi_wake_writer(readers, gate);
}

static bool rwbase_pi_finish_drain(atomic_t *readers)
{
	if (atomic_read_acquire(readers))
		return false;
	atomic_set(readers, RWBASE_PI_WRITER_BIAS);
	return true;
}

static void rwbase_pi_reopen(atomic_t *readers, int delta)
{
	/* Preserve concurrent reader exits; publish to fast-path readers. */
	atomic_add_return_release(delta, readers);
}

int __sched rwbase_pi_write_lock(atomic_t *readers, struct rt_mutex *gate, int state)
{
	int ret;

	ret = rt_mutex_lock_nolockdep(gate, state);
	if (ret)
		return ret;

	raw_spin_lock_irq(&gate->wait_lock);
	(void)atomic_sub_return(RWBASE_PI_READER_BIAS, readers);
	for (;;) {
		set_current_state(state);
		if (rwbase_pi_finish_drain(readers)) {
			__set_current_state(TASK_RUNNING);
			raw_spin_unlock_irq(&gate->wait_lock);
			return 0;
		}
		if (signal_pending_state(state, current))
			break;
		raw_spin_unlock_irq(&gate->wait_lock);
		schedule();
		raw_spin_lock_irq(&gate->wait_lock);
	}
	__set_current_state(TASK_RUNNING);
	rwbase_pi_reopen(readers, RWBASE_PI_READER_BIAS);
	raw_spin_unlock_irq(&gate->wait_lock);
	rt_mutex_unlock_nolockdep(gate);
	return -EINTR;
}

bool rwbase_pi_write_trylock(atomic_t *readers, struct rt_mutex *gate)
{
	unsigned long flags;
	bool locked;

	if (!rt_mutex_trylock_nolockdep(gate))
		return false;
	raw_spin_lock_irqsave(&gate->wait_lock, flags);
	(void)atomic_sub_return(RWBASE_PI_READER_BIAS, readers);
	locked = rwbase_pi_finish_drain(readers);
	if (!locked)
		rwbase_pi_reopen(readers, RWBASE_PI_READER_BIAS);
	raw_spin_unlock_irqrestore(&gate->wait_lock, flags);
	if (!locked)
		rt_mutex_unlock_nolockdep(gate);
	return locked;
}

void rwbase_pi_write_release(atomic_t *readers, struct rt_mutex *gate, bool downgrade)
{
	unsigned long flags;
	int delta = (int)((unsigned int)RWBASE_PI_READER_BIAS -
			  RWBASE_PI_WRITER_BIAS + downgrade);

	raw_spin_lock_irqsave(&gate->wait_lock, flags);
	rwbase_pi_reopen(readers, delta);
	raw_spin_unlock_irqrestore(&gate->wait_lock, flags);
	rt_mutex_unlock_nolockdep(gate);
}
