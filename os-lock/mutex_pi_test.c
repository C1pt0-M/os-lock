// SPDX-License-Identifier: GPL-2.0-only
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rtmutex.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>

#define PAIRS 100000

enum { L, M, H, WORKERS };
enum lock_mode { NATIVE, NATIVE_PI, PI_DYNAMIC, PI_STATIC, MODES };
static const char * const names[MODES] = { "mutex", "rt_mutex", "mutex_init_pi", "DEFINE_MUTEX_PI" };

static DEFINE_MUTEX_PI(static_lock);

struct trial {
	enum lock_mode mode;
	bool interference, io_nested, iowait_restored, recursive_busy;
	struct mutex dynamic_lock;
	struct mutex *mutex;
	struct rt_mutex rt;
	struct completion held, run_low, run_medium, ready_m, ready_h, done[WORKERS];
	int base, boosted, restored, medium_prio, high_prio;
	u64 pair_ns, low_start, low_unlock, medium_start, medium_end;
	u64 high_start, high_acquired;
};

static void lock(struct trial *s)
{
	if (s->mode == NATIVE_PI)
		rt_mutex_lock(&s->rt);
	else
		mutex_lock(s->mutex);
}

static void unlock(struct trial *s)
{
	if (s->mode == NATIVE_PI)
		rt_mutex_unlock(&s->rt);
	else
		mutex_unlock(s->mutex);
}

static int low(void *arg)
{
	struct trial *s = arg;
	u64 start;
	int i;

	sched_set_normal(current, 0);
	for (i = 0; i < 4096; i++) {
		lock(s);
		unlock(s);
	}
	start = ktime_get_ns();
	for (i = 0; i < PAIRS; i++) {
		lock(s);
		unlock(s);
	}
	s->pair_ns = ktime_get_ns() - start;
	lock(s);
	s->base = READ_ONCE(current->prio);
	complete(&s->held);
	wait_for_completion(&s->run_low);
	s->boosted = READ_ONCE(current->prio);
	s->low_start = ktime_get_ns();
	while (ktime_get_ns() - s->low_start < 5000000)
		cpu_relax();
	s->low_unlock = ktime_get_ns();
	unlock(s);
	s->restored = READ_ONCE(current->prio);
	complete(&s->done[L]);
	return 0;
}

static int medium(void *arg)
{
	struct trial *s = arg;

	sched_set_fifo_low(current);
	s->medium_prio = READ_ONCE(current->prio);
	complete(&s->ready_m);
	wait_for_completion(&s->run_medium);
	s->medium_start = ktime_get_ns();
	while (s->interference && ktime_get_ns() - s->medium_start < 30000000)
		cpu_relax();
	s->medium_end = ktime_get_ns();
	complete(&s->done[M]);
	return 0;
}

static int high(void *arg)
{
	struct trial *s = arg;

	sched_set_fifo(current);
	s->high_prio = READ_ONCE(current->prio);
	complete(&s->ready_h);
	wait_for_completion(&s->held);
	if (s->mode != NATIVE_PI)
		s->recursive_busy = mutex_trylock_recursive(s->mutex) == MUTEX_TRYLOCK_FAILED;
	/* The lower-priority workers share H's CPU and cannot preempt it. */
	if (s->interference)
		complete(&s->run_medium);
	complete(&s->run_low);
	s->high_start = ktime_get_ns();
	if (s->io_nested) {
		bool was_iowait = current->in_iowait;

		mutex_lock_io_nested(s->mutex, SINGLE_DEPTH_NESTING);
		s->iowait_restored = current->in_iowait == was_iowait;
	} else {
		lock(s);
	}
	s->high_acquired = ktime_get_ns();
	unlock(s);
	if (!s->interference)
		complete(&s->run_medium);
	complete(&s->done[H]);
	return 0;
}

static int run_trial(int round, enum lock_mode mode, bool interference, int cpu,
		     bool io_nested)
{
	static int (* const worker[WORKERS])(void *) = { low, medium, high };
	struct task_struct *tasks[WORKERS];
	struct trial s = {
		.mode = mode, .interference = interference, .io_nested = io_nested,
	};
	bool pi = mode == NATIVE_PI ||
		(IS_ENABLED(CONFIG_MUTEX_PI) && mode != NATIVE);
	int i, error = 0;
	bool pass;

	if (mode == NATIVE_PI) {
		rt_mutex_init(&s.rt);
	} else {
		s.mutex = mode == PI_STATIC ? &static_lock : &s.dynamic_lock;
		if (mode == NATIVE)
			mutex_init(s.mutex);
		else if (mode == PI_DYNAMIC)
			mutex_init_pi(s.mutex);

		if (mutex_is_locked(s.mutex) || !mutex_trylock(s.mutex))
			return -EINVAL;
		pass = mutex_is_locked(s.mutex) && !mutex_trylock(s.mutex);
		mutex_unlock(s.mutex);
		if (!pass || mutex_is_locked(s.mutex))
			return -EINVAL;
		if (mutex_trylock_recursive(s.mutex) != MUTEX_TRYLOCK_SUCCESS)
			return -EINVAL;
		pass = mutex_trylock_recursive(s.mutex) == MUTEX_TRYLOCK_RECURSIVE;
		mutex_unlock(s.mutex);
		if (!pass || mutex_is_locked(s.mutex))
			return -EINVAL;
	}
	init_completion(&s.held);
	init_completion(&s.run_low);
	init_completion(&s.run_medium);
	init_completion(&s.ready_m);
	init_completion(&s.ready_h);
	for (i = 0; i < WORKERS; i++) {
		init_completion(&s.done[i]);
		tasks[i] = kthread_create(worker[i], &s, "mpi_test_%c", "LMH"[i]);
		if (IS_ERR(tasks[i])) {
			error = PTR_ERR(tasks[i]);
			goto release;
		}
		get_task_struct(tasks[i]);
		kthread_bind(tasks[i], cpu);
	}
	/* Establish M/H's FIFO policies before L can open H's gate. */
	wake_up_process(tasks[H]);
	wake_up_process(tasks[M]);
	wait_for_completion(&s.ready_h);
	wait_for_completion(&s.ready_m);
	wake_up_process(tasks[L]);
	wait_for_completion(&s.done[H]);
	wait_for_completion(&s.done[M]);
	wait_for_completion(&s.done[L]);
release:
	while (i--) {
		kthread_stop(tasks[i]);
		put_task_struct(tasks[i]);
	}
	if (mode == NATIVE_PI)
		rt_mutex_destroy(&s.rt);
	else if (mode != PI_STATIC)
		mutex_destroy(s.mutex);
	if (error)
		return error;
	pass = s.high_start && s.high_start <= s.low_start &&
		s.high_start <= s.medium_start && s.low_start <= s.low_unlock &&
		s.low_unlock <= s.high_acquired && s.medium_start <= s.medium_end &&
		(mode == NATIVE_PI || s.recursive_busy) &&
		s.high_prio < s.medium_prio && s.medium_prio < s.base &&
		s.restored == s.base && s.boosted == (pi ? s.high_prio : s.base) &&
		((!interference || pi) ? s.high_acquired <= s.medium_start
				     : s.medium_end <= s.low_start);
	if (io_nested) {
		pass &= s.iowait_restored;
		pr_info("MPT_IO_NESTED,%s,%d,%d,%d,%d,%s\n", names[mode],
			s.base, s.boosted, s.restored, s.iowait_restored,
			pass ? "PASS" : "FAIL");
		return pass ? 0 : -EINVAL;
	}
	pr_info("MPT_RESULT,%d,%s,%d,%d,%llu,%llu,%d,%d,%d,%s\n", round,
		names[mode], interference, cpu, s.high_acquired - s.high_start,
		s.pair_ns, s.base, s.boosted, s.restored, pass ? "PASS" : "FAIL");
	return pass ? 0 : -EINVAL;
}

/* Both the owner (insmod) and its peers run on the same guest CPU. */
static struct task_struct *make_peer(int (*fn)(void *), void *arg, int cpu, bool high)
{
	struct task_struct *task = kthread_create(fn, arg, "mpi_peer");

	if (IS_ERR(task))
		return task;
	get_task_struct(task);
	kthread_bind(task, cpu);
	if (high)
		sched_set_fifo(task);
	else
		sched_set_fifo_low(task);
	return task;
}

static void join_peer(struct task_struct *task)
{
	kthread_stop(task);
	put_task_struct(task);
}

struct chain_trial {
	struct mutex a, b;
	struct completion middle_started, high_started;
	int middle_base, middle_boost, middle_after, high_prio;
};

static int chain_middle(void *arg)
{
	struct chain_trial *s = arg;

	mutex_lock(&s->a);
	s->middle_base = READ_ONCE(current->prio);
	complete(&s->middle_started);
	mutex_lock(&s->b);
	s->middle_boost = READ_ONCE(current->prio);
	mutex_unlock(&s->b);
	mutex_unlock(&s->a);
	s->middle_after = READ_ONCE(current->prio);
	return 0;
}

static int chain_high(void *arg)
{
	struct chain_trial *s = arg;

	s->high_prio = READ_ONCE(current->prio);
	complete(&s->high_started);
	mutex_lock(&s->a);
	mutex_unlock(&s->a);
	return 0;
}

static int check_chain(int cpu)
{
	struct chain_trial s;
	struct task_struct *middle, *high;
	int owner_base, owner_step1, owner_step2, owner_after;
	bool pi = IS_ENABLED(CONFIG_MUTEX_PI), pass;

	mutex_init_pi(&s.a);
	mutex_init_pi(&s.b);
	init_completion(&s.middle_started);
	init_completion(&s.high_started);
	middle = make_peer(chain_middle, &s, cpu, false);
	if (IS_ERR(middle))
		return PTR_ERR(middle);
	high = make_peer(chain_high, &s, cpu, true);
	if (IS_ERR(high)) {
		join_peer(middle);
		return PTR_ERR(high);
	}
	mutex_lock(&s.b);
	owner_base = READ_ONCE(current->prio);
	/* Each higher-priority peer reaches its blocking lock before we resume. */
	wake_up_process(middle);
	wait_for_completion(&s.middle_started);
	owner_step1 = READ_ONCE(current->prio);
	wake_up_process(high);
	wait_for_completion(&s.high_started);
	owner_step2 = READ_ONCE(current->prio);
	mutex_unlock(&s.b);
	owner_after = READ_ONCE(current->prio);
	join_peer(high);
	join_peer(middle);
	mutex_destroy(&s.b);
	mutex_destroy(&s.a);
	pass = owner_step1 == (pi ? s.middle_base : owner_base) &&
		owner_step2 == (pi ? s.high_prio : owner_base) &&
		owner_after == owner_base &&
		s.middle_boost == (pi ? s.high_prio : s.middle_base) &&
		s.middle_after == s.middle_base;
	pr_info("MPT_CHAIN,%d,%d,%d,%d,%d,%d,%d,%d,%s\n", owner_base,
		owner_step1, owner_step2, owner_after, s.middle_base,
		s.middle_boost, s.middle_after, s.high_prio, pass ? "PASS" : "FAIL");
	return pass ? 0 : -EINVAL;
}

#ifdef CONFIG_SMP
struct cross_cpu_trial {
	struct mutex lock;
	struct completion started, acquired;
	int waiter_cpu, waiter_prio;
};

static int cross_cpu_waiter(void *arg)
{
	struct cross_cpu_trial *s = arg;

	s->waiter_cpu = task_cpu(current);
	s->waiter_prio = READ_ONCE(current->prio);
	complete(&s->started);
	mutex_lock(&s->lock);
	complete(&s->acquired);
	mutex_unlock(&s->lock);
	return 0;
}

static int check_cross_cpu(int cpu, int remote, int round, bool pi)
{
	struct cross_cpu_trial s = { .waiter_cpu = -1 };
	struct task_struct *waiter;
	unsigned long deadline;
	int owner_cpu, before, held, after;
	bool blocked = false, pass;

	if (pi)
		mutex_init_pi(&s.lock);
	else
		mutex_init(&s.lock);
	init_completion(&s.started);
	init_completion(&s.acquired);
	waiter = make_peer(cross_cpu_waiter, &s, remote, true);
	if (IS_ERR(waiter)) {
		mutex_destroy(&s.lock);
		return PTR_ERR(waiter);
	}

	mutex_lock(&s.lock);
	owner_cpu = task_cpu(current);
	before = READ_ONCE(current->prio);
	wake_up_process(waiter);
	if (wait_for_completion_timeout(&s.started, HZ)) {
		deadline = jiffies + HZ;
		do {
			/* on_cpu is cleared with release semantics at switch-out. */
			if (READ_ONCE(waiter->state) == TASK_UNINTERRUPTIBLE &&
			    !smp_load_acquire(&waiter->on_cpu)) {
				blocked = !completion_done(&s.acquired);
				break;
			}
			usleep_range(100, 200);
		} while (time_before(jiffies, deadline));
	}
	held = READ_ONCE(current->prio);
	mutex_unlock(&s.lock);
	after = READ_ONCE(current->prio);
	join_peer(waiter);
	mutex_destroy(&s.lock);
	pass = blocked && owner_cpu == cpu && s.waiter_cpu == remote &&
		cpu != remote && s.waiter_prio < before &&
		held == (pi && IS_ENABLED(CONFIG_MUTEX_PI) ? s.waiter_prio : before) &&
		after == before && completion_done(&s.acquired);
	pr_info("MPT_CROSS_CPU,%d,%s,%d,%d,%d,%d,%d,%d,%s\n", round,
		pi ? "pi" : "ordinary", owner_cpu, s.waiter_cpu, before, held,
		after, s.waiter_prio, pass ? "PASS" : "FAIL");
	return pass ? 0 : -EINVAL;
}
#endif

struct signal_trial {
	struct mutex lock;
	struct completion started, done;
	bool killable;
	int ret, prio;
};

static int signal_waiter(void *arg)
{
	struct signal_trial *s = arg;

	allow_signal(SIGUSR1);
	allow_signal(SIGKILL);
	s->prio = READ_ONCE(current->prio);
	complete(&s->started);
	s->ret = s->killable ? mutex_lock_killable(&s->lock) :
		mutex_lock_interruptible(&s->lock);
	if (!s->ret)
		mutex_unlock(&s->lock);
	complete(&s->done);
	return 0;
}

static int check_signal(int cpu, bool killable)
{
	struct signal_trial s = { .killable = killable };
	struct task_struct *waiter;
	int base, boosted, after, target_state, sent;
	bool early = false, done, held, pass;

	mutex_init_pi(&s.lock);
	init_completion(&s.started);
	init_completion(&s.done);
	waiter = make_peer(signal_waiter, &s, cpu, true);
	if (IS_ERR(waiter))
		return PTR_ERR(waiter);
	mutex_lock(&s.lock);
	base = READ_ONCE(current->prio);
	wake_up_process(waiter);
	wait_for_completion(&s.started);
	boosted = READ_ONCE(current->prio);
	target_state = READ_ONCE(waiter->state);
	sent = send_sig(SIGUSR1, waiter, 1);
	if (!sent && killable) {
		early = wait_for_completion_timeout(&s.done, msecs_to_jiffies(30));
		if (!early)
			sent = send_sig(SIGKILL, waiter, 1);
	}
	done = !sent && !early && wait_for_completion_timeout(&s.done, HZ);
	after = READ_ONCE(current->prio);
	held = mutex_is_locked(&s.lock);
	mutex_unlock(&s.lock);
	join_peer(waiter);
	pass = done && held && s.ret == -EINTR && after == base &&
		target_state == (killable ? TASK_KILLABLE : TASK_INTERRUPTIBLE) &&
		boosted == (IS_ENABLED(CONFIG_MUTEX_PI) ? s.prio : base);
	mutex_destroy(&s.lock);
	pr_info("MPT_SIGNAL,%s,%d,%d,%d,%d,%d,%d,%s\n",
		killable ? "killable" : "interruptible", base, boosted, after,
		s.ret, held, early, pass ? "PASS" : "FAIL");
	return pass ? 0 : -EINVAL;
}

#define COST_BATCHES 31

static void check_uncontended_cost(void)
{
	static const enum lock_mode cost_modes[] = { NATIVE, PI_DYNAMIC, NATIVE_PI };
	struct trial s = { .mutex = &s.dynamic_lock };
	struct mutex pi;
	int batch, mode, i;

	mutex_init(&s.dynamic_lock);
	mutex_init_pi(&pi);
	rt_mutex_init(&s.rt);
	for (batch = 0; batch < COST_BATCHES; batch++) {
		for (mode = 0; mode < ARRAY_SIZE(cost_modes); mode++) {
			unsigned long switches;
			u64 start, elapsed;

			s.mode = cost_modes[(batch + mode) % ARRAY_SIZE(cost_modes)];
			s.mutex = s.mode == PI_DYNAMIC ? &pi : &s.dynamic_lock;
			for (i = 0; i < 4096; i++) {
				lock(&s);
				unlock(&s);
			}
			switches = current->nvcsw + current->nivcsw;
			start = ktime_get_ns();
			for (i = 0; i < PAIRS; i++) {
				lock(&s);
				unlock(&s);
			}
			elapsed = ktime_get_ns() - start;
			pr_info("MPT_COST,%d,%s,%d,%llu,%lu\n", batch + 1,
				names[s.mode], PAIRS, elapsed,
				current->nvcsw + current->nivcsw - switches);
		}
	}
	mutex_destroy(&s.dynamic_lock);
	mutex_destroy(&pi);
	rt_mutex_destroy(&s.rt);
}

static int __init mutex_pi_test_init(void)
{
	int round, i, err;
	int cpu = cpumask_first(cpu_online_mask);
	cpumask_var_t original;

	pr_info("MPT_CONFIG,%d,%zu,%zu\n", IS_ENABLED(CONFIG_MUTEX_PI),
		sizeof(struct mutex), sizeof(struct rt_mutex));
	for (round = 1; round <= 5; round++) {
		for (i = 0; i < 2 * MODES; i++) {
			int slot = (i + round - 1) % (2 * MODES);

			msleep(1000);
			err = run_trial(round, slot / 2, slot % 2, cpu, false);
			if (err)
				return err;
		}
	}
	if (IS_ENABLED(CONFIG_DEBUG_LOCK_ALLOC)) {
		msleep(1000);
		err = run_trial(0, NATIVE, true, cpu, true);
		if (err)
			return err;
		msleep(1000);
		err = run_trial(0, PI_DYNAMIC, true, cpu, true);
		if (err)
			return err;
	}
	if (!alloc_cpumask_var(&original, GFP_KERNEL))
		return -ENOMEM;
	cpumask_copy(original, current->cpus_ptr);
	err = set_cpus_allowed_ptr(current, cpumask_of(cpu));
	if (!err) {
		err = check_chain(cpu);
		if (!err)
			err = check_signal(cpu, false);
		if (!err)
			err = check_signal(cpu, true);
#ifdef CONFIG_SMP
		if (!err && num_online_cpus() > 1) {
			int remote = cpumask_next(cpu, cpu_online_mask);

			for (round = 1; round <= 12 && !err; round++) {
				err = check_cross_cpu(cpu, remote, round, false);
				if (!err)
					err = check_cross_cpu(cpu, remote, round, true);
			}
		}
#endif
		if (!err)
			check_uncontended_cost();
		set_cpus_allowed_ptr(current, original);
	}
	free_cpumask_var(original);
	return err;
}

static void __exit mutex_pi_test_exit(void) { }
module_init(mutex_pi_test_init);
module_exit(mutex_pi_test_exit);
MODULE_LICENSE("GPL");
