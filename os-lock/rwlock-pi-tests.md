# Task-context PI rwlock acceptance fixtures

The feature is independent of MUTEX_PI and RWSEM_PI. CONFIG_RWLOCK_PI=y
requires RT_MUTEXES and PREEMPT_COUNT. Include linux/rwlock_pi.h; initialize
with rwlock_init_pi(), DEFINE_RWLOCK_PI(), or __RWLOCK_PI_UNLOCKED().
Only pi_read_lock, pi_write_lock, pi_read_trylock, pi_write_trylock,
pi_read_unlock and pi_write_unlock are supplied. No BH/IRQ variants exist.

PI calls require TASK_RUNNING sleepable task context, enabled interrupts,
and no preempt-disable or interrupt nesting. Migration is allowed. There
is no implicit CPU-local or RCU protection, recursive same-instance read,
upgrade or downgrade API. A successful acquisition is released by the same
task. Anonymous reader ownership is not tracked in non-Lockdep builds.

Invalid-context try calls WARN and return zero without acquisition. Invalid
void acquisitions and write unlock by a non-owner WARN then BUG rather than
returning to an unprotected critical section. With Lockdep active, same
instance reacquisition is rejected before enqueueing. Run misuse tests in
separate guests; their expected warnings/BUGs are not positive-suite passes.

CONFIG_RWLOCK_PI=n aliases the type and six operations to native rwlock_t.
Such critical sections cannot sleep. Business code intended for both
configurations must obey their common subset. Three-sleeping-lock chain
tests only execute with MUTEX_PI, RWSEM_PI and RWLOCK_PI all enabled.

## Module parameters

Build rwlock_pi_test.ko against each exact guest kernel, never the host.
kind=0 selects native rwlock_t; kind=1 selects rwlock_pi_t (native fallback
when disabled). suite=0 runs functional, try rollback, migration, gate PI
and DRAIN-boundary checks. suite=1 exercises all six three-type orderings,
with same/cross CPU placement and root/two-middle priority restoration.
suite=2 runs eight workers, 4R4W for at least twenty seconds; suite=3 runs
31 measured rounds of 100,000 uncontended pairs and four throughput mixes
of 25,000 operations per worker. suite=4 runs four inversion/response
scenarios; native critical sections use fixed CPU busy work and never
sleep or wait for another task while holding the lock. suite=6 prints
type sizes for guest Slab comparison.

suite=5 negative=1/7/8/9/10 tests try failure under preempt-disable, IRQ-off,
non-RUNNING, hardirq and BH-disabled contexts respectively. negative=2/3/4
provokes void-acquire fail-stop under atomic/IRQ-off/non-RUNNING contexts;
negative=5 is a Lockdep-held recursive read; negative=6 is non-owner write
unlock. Each case needs its own guest.

RLP_INV columns: kind, group, H_write, M_enabled, trial, control_wake_to_done_ns,
H_call_to_acquire_ns, H_cpu_ns, owner_cpu_ns, owner_wall_ns, owner_base_prio,
owner_before_unlock_prio, owner_after_unlock_prio. Group0 is same-CPU writer,
group1 cross-CPU writer, group2 reader-owner/writer-H DRAIN, group3 an
unrelated high-priority task. A zero call-to-acquire value in group3 means
no lock request, not zero scheduling delay.

The external results runner records full serial logs, exact commands,
matching configs/images/modules and SHA-256. Formal performance must run
only after all host compilation finishes. KernelAddressSanitizer and KCSAN
are deliberately deferred pending code/data acceptance.
