/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_LOCKING_RWBASE_PI_H
#define _KERNEL_LOCKING_RWBASE_PI_H
#include <linux/atomic.h>
#include <linux/rtmutex.h>
#define RWBASE_PI_READER_BIAS (-2147483647 - 1)
#define RWBASE_PI_WRITER_BIAS (1 << 30)
bool rwbase_pi_read_trylock(atomic_t *readers, struct rt_mutex *gate);
int rwbase_pi_read_lock(atomic_t *readers, struct rt_mutex *gate, int state);
void rwbase_pi_read_unlock(atomic_t *readers, struct rt_mutex *gate);
int rwbase_pi_write_lock(atomic_t *readers, struct rt_mutex *gate, int state);
bool rwbase_pi_write_trylock(atomic_t *readers, struct rt_mutex *gate);
void rwbase_pi_write_release(atomic_t *readers, struct rt_mutex *gate, bool downgrade);
#endif
