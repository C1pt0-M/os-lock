// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/rwlock_pi.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched/cputime.h>
#include <linux/sched/task.h>
#include <linux/fs.h>
#include <linux/mm_types.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <net/sock.h>

static int suite, kind = 1, negative;
module_param(suite, int, 0444);
module_param(kind, int, 0444);
module_param(negative, int, 0444);
static DEFINE_RWLOCK_PI(static_lock);
static struct {
	rwlock_pi_t lock;
} member = { .lock = __RWLOCK_PI_UNLOCKED(member.lock) };
struct test_lock {
	rwlock_t native;
	rwlock_pi_t pi;
};
static void init_lock(struct test_lock *l)
{
	rwlock_init(&l->native);
	rwlock_init_pi(&l->pi);
}
static void take(struct test_lock *l, bool write)
{
	if (kind) {
		if (write)
			pi_write_lock(&l->pi);
		else
			pi_read_lock(&l->pi);
	} else {
		if (write)
			write_lock(&l->native);
		else
			read_lock(&l->native);
	}
}
static void drop(struct test_lock *l, bool write)
{
	if (kind) {
		if (write)
			pi_write_unlock(&l->pi);
		else
			pi_read_unlock(&l->pi);
	} else {
		if (write)
			write_unlock(&l->native);
		else
			read_unlock(&l->native);
	}
}
static int attempt(struct test_lock *l, bool write)
{
	return kind ? (write ? pi_write_trylock(&l->pi) :
			       pi_read_trylock(&l->pi)) :
		      (write ? write_trylock(&l->native) :
			       read_trylock(&l->native));
}
static void busy(u64 ns)
{
	u64 start = ktime_get_ns();
	while (ktime_get_ns() - start < ns)
		cpu_relax();
}
static int check(const char *name, bool ok)
{
	pr_info("RLP_CASE,%s,%s\n", name, ok ? "PASS" : "FAIL");
	return ok ? 0 : -EINVAL;
}

struct worker {
	struct test_lock *lock;
	struct task_struct *task;
	struct completion ready, start, done, release;
	int cpu, priority, at_lock, after;
	bool write, probe, medium, high, hold;
	int read_try, write_try;
	u64 wake_ns, request, acquired, finished, cpu_ns;
};
static int worker_fn(void *arg)
{
	struct worker *w = arg;
	u64 u0, k0, u1, k1;
	if (w->medium)
		sched_set_fifo_low(current);
	if (w->high)
		sched_set_fifo(current);
	w->priority = READ_ONCE(current->prio);
	complete(&w->ready);
	wait_for_completion(&w->start);
	task_cputime_adjusted(current, &u0, &k0);
	w->request = ktime_get_ns();
	if (w->probe) {
		w->read_try = attempt(w->lock, false);
		if (w->read_try)
			drop(w->lock, false);
		w->write_try = attempt(w->lock, true);
		if (w->write_try)
			drop(w->lock, true);
	} else if (w->medium)
		busy(12000000);
	else if (w->lock) {
		take(w->lock, w->write);
		w->acquired = ktime_get_ns();
		w->at_lock = READ_ONCE(current->prio);
		if (w->hold) {
			complete(&w->done);
			wait_for_completion(&w->release);
		}
		drop(w->lock, w->write);
		w->after = READ_ONCE(current->prio);
	}
	w->finished = ktime_get_ns();
	task_cputime_adjusted(current, &u1, &k1);
	w->cpu_ns = u1 - u0 + k1 - k0;
	complete(&w->done);
	return 0;
}
static int spawn(struct worker *w, int cpu)
{
	init_completion(&w->ready);
	init_completion(&w->start);
	init_completion(&w->done);
	init_completion(&w->release);
	w->cpu = cpu;
	w->task = kthread_create(worker_fn, w, "rwlock_peer");
	if (IS_ERR(w->task))
		return PTR_ERR(w->task);
	get_task_struct(w->task);
	kthread_bind(w->task, cpu);
	wake_up_process(w->task);
	return wait_for_completion_timeout(&w->ready, 2 * HZ) ? 0 : -ETIMEDOUT;
}
static void stop(struct worker *w)
{
	if (!w->task || IS_ERR(w->task))
		return;
	complete(&w->start);
	complete(&w->release);
	kthread_stop(w->task);
	put_task_struct(w->task);
}
#ifdef CONFIG_RWLOCK_PI
static bool wait_blocked(struct task_struct *task)
{
	unsigned long until = jiffies + 2 * HZ;
	do {
		if (READ_ONCE(task->state) == TASK_UNINTERRUPTIBLE &&
		    !smp_load_acquire(&task->on_cpu))
			return true;
		usleep_range(100, 200);
	} while (time_before(jiffies, until));
	return false;
}

#endif
static int functional(void)
{
	struct test_lock l;
	struct worker p = { .probe = true, .lock = &l };
	int ret, base = preempt_count(), other = cpumask_first(cpu_online_mask);
	bool ok;
	pi_read_lock(&static_lock);
	pi_read_unlock(&static_lock);
	pi_write_lock(&member.lock);
	pi_write_unlock(&member.lock);
	init_lock(&l);
	ret = check("initializers", preempt_count() == base);
	if (ret)
		return ret;
	ret = spawn(&p, other);
	if (ret)
		return ret;
	take(&l, false);
	complete(&p.start);
	/* Native holder cannot sleep: the remote probe must run elsewhere. */
	if (IS_ENABLED(CONFIG_RWLOCK_PI) && kind) {
		wait_for_completion(&p.done);
	} else {
		drop(&l, false);
		wait_for_completion(&p.done);
		take(&l, false);
	}
	drop(&l, false);
	stop(&p);
	ok = p.read_try &&
	     (IS_ENABLED(CONFIG_RWLOCK_PI) && kind ? !p.write_try : true);
	ret = check("read_try_and_writer_rollback", ok);
	if (ret)
		return ret;
	ok = attempt(&l, true);
	if (ok)
		drop(&l, true);
	ret = check("write_try_reuse", ok);
	if (ret)
		return ret;
#ifdef CONFIG_RWLOCK_PI
	if (kind) {
		struct worker r = { .lock = &l, .hold = true };
		int first = task_cpu(current),
		    target = num_online_cpus() > 1 ?
				     cpumask_next(first, cpu_online_mask) :
				     first;
		ret = spawn(&r, target);
		if (ret)
			return ret;
		take(&l, false);
		complete(&r.start);
		ok = wait_for_completion_timeout(&r.done, 2 * HZ) &&
		     atomic_read(&l.pi.readers) == (-2147483647 - 1) + 2;
		drop(&l, false);
		stop(&r);
		ret = check("parallel_readers", ok);
		if (ret)
			return ret;
		pi_write_lock(&l.pi);
		memset(&p, 0, sizeof(p));
		p.probe = true;
		p.lock = &l;
		ret = spawn(&p, target);
		if (ret)
			return ret;
		complete(&p.start);
		wait_for_completion(&p.done);
		ok = !p.read_try && !p.write_try;
		pi_write_unlock(&l.pi);
		stop(&p);
		ret = check("write_excludes_try", ok);
		if (ret)
			return ret;
		if (num_online_cpus() > 1) {
			cpumask_var_t saved;
			if (!alloc_cpumask_var(&saved, GFP_KERNEL))
				return -ENOMEM;
			cpumask_copy(saved, current->cpus_ptr);
			set_cpus_allowed_ptr(current, cpu_online_mask);
			pi_read_lock(&l.pi);
			first = task_cpu(current);
			target = cpumask_any_but(cpu_online_mask, first);
			set_cpus_allowed_ptr(current, cpumask_of(target));
			ok = task_cpu(current) == target && target != first &&
			     !in_atomic();
			pi_read_unlock(&l.pi);
			set_cpus_allowed_ptr(current, cpu_online_mask);
			pi_write_lock(&l.pi);
			first = task_cpu(current);
			target = cpumask_any_but(cpu_online_mask, first);
			set_cpus_allowed_ptr(current, cpumask_of(target));
			ok &= task_cpu(current) == target && !in_atomic();
			pi_write_unlock(&l.pi);
			set_cpus_allowed_ptr(current, saved);
			free_cpumask_var(saved);
			ret = check("migration_while_read_and_write_held", ok);
			if (ret)
				return ret;
		}
	}
#endif
	return check("context_balanced",
		     preempt_count() == base && !irqs_disabled());
}

struct load_shared {
	struct test_lock lock;
	struct completion start;
	atomic_t readers, writers;
	u64 start_ns;
	bool pressure;
	int nr;
};
struct load_worker {
	struct load_shared *s;
	struct task_struct *task;
	struct completion done;
	int id, cpu, peak, bad;
	u64 ops, maxwait, cpu_ns;
};
static int load_fn(void *arg)
{
	struct load_worker *w = arg;
	struct load_shared *s = w->s;
	bool write = w->id >= s->nr;
	u64 u0, k0, u1, k1;
	wait_for_completion(&s->start);
	task_cputime_adjusted(current, &u0, &k0);
	while (s->pressure ?
		       ktime_get_ns() - s->start_ns < 20ULL * NSEC_PER_SEC :
		       w->ops < 25000) {
		u64 requested = ktime_get_ns();
		take(&s->lock, write);
		w->maxwait = max(w->maxwait, ktime_get_ns() - requested);
		if (write) {
			if (atomic_inc_return(&s->writers) != 1 ||
			    atomic_read(&s->readers))
				w->bad++;
		} else {
			w->peak = max(w->peak, atomic_inc_return(&s->readers));
			if (atomic_read(&s->writers))
				w->bad++;
		}
		busy(2500);
		if (write)
			atomic_dec(&s->writers);
		else
			atomic_dec(&s->readers);
		drop(&s->lock, write);
		w->ops++;
		cond_resched();
	}
	task_cputime_adjusted(current, &u1, &k1);
	w->cpu_ns = u1 - u0 + k1 - k0;
	complete(&w->done);
	return 0;
}
static int pressure_or_load(bool pressure)
{
	const int mixes[] = { 8, 7, 4, 0 };
	int mix;
	for (mix = 0; mix < (pressure ? 1 : 4); mix++) {
		struct load_shared s = { .pressure = pressure,
					 .nr = pressure ? 4 : mixes[mix] };
		struct load_worker w[8] = {};
		int i, cpu = 0, n, bad = 0, peak = 0;
		u64 elapsed, total = 0, cpu_total = 0;
		init_lock(&s.lock);
		init_completion(&s.start);
		for (i = 0; i < 8; i++) {
			w[i].s = &s;
			w[i].id = i;
			w[i].cpu = cpu;
			init_completion(&w[i].done);
			w[i].task = kthread_create(load_fn, &w[i],
						   "rlp_load/%d", i);
			if (IS_ERR(w[i].task))
				break;
			get_task_struct(w[i].task);
			kthread_bind(w[i].task, cpu);
			wake_up_process(w[i].task);
			cpu = cpumask_next(cpu, cpu_online_mask);
			if (cpu >= nr_cpu_ids)
				cpu = 0;
		}
		n = i;
		s.start_ns = ktime_get_ns();
		complete_all(&s.start);
		for (i = 0; i < n; i++)
			wait_for_completion(&w[i].done);
		elapsed = ktime_get_ns() - s.start_ns;
		for (i = 0; i < n; i++) {
			kthread_stop(w[i].task);
			put_task_struct(w[i].task);
			total += w[i].ops;
			cpu_total += w[i].cpu_ns;
			bad += w[i].bad + !w[i].ops;
			peak = max(peak, w[i].peak);
			pr_info("RLP_WORK,%d,%d,%d,%d,%llu,%llu,%llu,%d\n",
				kind, s.nr, i, w[i].cpu, w[i].ops, w[i].cpu_ns,
				w[i].maxwait, w[i].bad);
		}
		bad += atomic_read(&s.readers) != 0 ||
		       atomic_read(&s.writers) != 0 || n != 8;
		pr_info("RLP_LOAD,%d,%d,%d,%llu,%llu,%llu,%d,%d\n", kind,
			pressure, s.nr, elapsed, total, cpu_total, peak, bad);
		if (bad)
			return -EINVAL;
	}
	return 0;
}
static int uncontended(void)
{
	struct test_lock l;
	int round, write, i;
	u64 start;
	init_lock(&l);
	for (round = 0; round <= 31; round++)
		for (write = 0; write < 2; write++) {
			start = ktime_get_ns();
			for (i = 0; i < 100000; i++) {
				take(&l, write);
				drop(&l, write);
			}
			start = ktime_get_ns() - start;
			if (round)
				pr_info("RLP_PAIR,%d,%d,%d,100000,%llu\n", kind,
					round, write, start);
		}
	return 0;
}
static void cpu_work(u64 ns)
{
	u64 u0, k0, u1, k1;
	task_cputime_adjusted(current, &u0, &k0);
	do {
		cpu_relax();
		task_cputime_adjusted(current, &u1, &k1);
	} while (u1 - u0 + k1 - k0 < ns);
}
static int inversion(void)
{
	int group, write, m, round;
	for (group = 0; group < 4; group++) {
		if (group == 1 && num_online_cpus() < 2)
			continue;
		for (write = 0; write < 2; write++) {
			if ((group == 2 && !write) || (group == 3 && write))
				continue;
			for (m = 0; m < 2; m++)
				for (round = 0; round < 6; round++) {
					struct test_lock l;
					struct worker h = { .write = write,
							    .high = true },
						      medium = { .medium =
									 true };
					int ret, base, boosted, after;
					bool owner_write = group != 2;
					u64 control, release, u0, k0, u1, k1;
					init_lock(&l);
					h.lock = group == 3 ? NULL : &l;
					ret = spawn(&h, group == 1 ? 1 : 0);
					if (ret)
						return ret;
					if (m) {
						ret = spawn(&medium, 0);
						if (ret) {
							stop(&h);
							return ret;
						}
					}
					base = READ_ONCE(current->prio);
					take(&l, owner_write);
					task_cputime_adjusted(current, &u0,
							      &k0);
					control = ktime_get_ns();
					complete(&h.start);
					if (m)
						complete(&medium.start);
					cpu_work(2000000);
					boosted = READ_ONCE(current->prio);
					release = ktime_get_ns();
					drop(&l, owner_write);
					after = READ_ONCE(current->prio);
					task_cputime_adjusted(current, &u1,
							      &k1);
					wait_for_completion(&h.done);
					if (m)
						wait_for_completion(
							&medium.done);
					stop(&h);
					if (m)
						stop(&medium);
					pr_info("RLP_INV,%d,%d,%d,%d,%d,%llu,%llu,%llu,%llu,%llu,%d,%d,%d\n",
						kind, group, write, m, round,
						h.finished - control,
						h.acquired ?
							h.acquired - h.request :
							0,
						h.cpu_ns, u1 - u0 + k1 - k0,
						release - control, base,
						boosted, after);
					if (after != base)
						return -EINVAL;
				}
		}
	}
	return 0;
}
#if defined(CONFIG_RWLOCK_PI) && defined(CONFIG_RWSEM_PI) &&                   \
	defined(CONFIG_MUTEX_PI)
struct chain {
	struct mutex mutex;
	struct rw_semaphore sem;
	rwlock_pi_t rw;
	int order[3];
};
static void chain_take(struct chain *s, int type)
{
	if (type == 0)
		mutex_lock(&s->mutex);
	else if (type == 1)
		down_write(&s->sem);
	else
		pi_write_lock(&s->rw);
}
static void chain_drop(struct chain *s, int type)
{
	if (type == 0)
		mutex_unlock(&s->mutex);
	else if (type == 1)
		up_write(&s->sem);
	else
		pi_write_unlock(&s->rw);
}
struct chain_peer {
	struct chain *s;
	struct task_struct *task;
	struct completion ready, done;
	int level, cpu, base, boost, after_inner, after_outer;
};
static int chain_fn(void *arg)
{
	struct chain_peer *p = arg;
	int own = p->level;
	if (own == 2)
		sched_set_fifo(current);
	else
		sched_set_fifo_low(current);
	p->base = READ_ONCE(current->prio);
	if (own < 2)
		chain_take(p->s, p->s->order[own + 1]);
	complete(&p->ready);
	chain_take(p->s, p->s->order[own]);
	p->boost = READ_ONCE(current->prio);
	chain_drop(p->s, p->s->order[own]);
	p->after_inner = READ_ONCE(current->prio);
	if (own < 2)
		chain_drop(p->s, p->s->order[own + 1]);
	p->after_outer = READ_ONCE(current->prio);
	complete(&p->done);
	return 0;
}
static int chains(void)
{
	const int orders[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 },
				   { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
	int n, placement;
	for (n = 0; n < 6; n++)
		for (placement = 0; placement < 2; placement++) {
			struct chain s;
			struct chain_peer p[3] = {};
			int i, base, boost, after;
			bool ok = true;
			/* Each permutation has an independent class hierarchy. */
			static struct lock_class_key keys[6][3];
			__mutex_init_pi(&s.mutex, "rlp_chain_mutex",
					&keys[n][0]);
			__init_rwsem_pi(&s.sem, "rlp_chain_sem", &keys[n][1]);
			__rwlock_init_pi(&s.rw, "rlp_chain_rw", &keys[n][2]);
			memcpy(s.order, orders[n], sizeof(s.order));
			base = READ_ONCE(current->prio);
			chain_take(&s, s.order[0]);
			for (i = 0; i < 3; i++) {
				p[i].s = &s;
				p[i].level = i;
				p[i].cpu = placement && num_online_cpus() > 1 ?
						   1 :
						   0;
				init_completion(&p[i].ready);
				init_completion(&p[i].done);
				p[i].task = kthread_create(chain_fn, &p[i],
							   "rlp_chain/%d", i);
				if (IS_ERR(p[i].task))
					return PTR_ERR(p[i].task);
				get_task_struct(p[i].task);
				kthread_bind(p[i].task, p[i].cpu);
				wake_up_process(p[i].task);
				ok &= wait_for_completion_timeout(&p[i].ready,
								  2 * HZ) != 0;
				ok &= wait_blocked(p[i].task);
			}
			boost = READ_ONCE(current->prio);
			ok &= boost == 49 && READ_ONCE(p[0].task->prio) == 49 &&
			      READ_ONCE(p[1].task->prio) == 49;
			chain_drop(&s, s.order[0]);
			after = READ_ONCE(current->prio);
			for (i = 0; i < 3; i++) {
				ok &= wait_for_completion_timeout(&p[i].done,
								  2 * HZ) != 0;
				kthread_stop(p[i].task);
				put_task_struct(p[i].task);
			}
			ok &= after == base && p[0].boost == 49 &&
			      p[1].boost == 49 && p[0].after_inner == 49 &&
			      p[1].after_inner == 49 &&
			      p[0].after_outer == p[0].base &&
			      p[1].after_outer == p[1].base;
			pr_info("RLP_CHAIN,%d,%d,root=%d/%d/%d,L1=%d/%d/%d/%d,L2=%d/%d/%d/%d,%s\n",
				n, placement, base, boost, after, p[0].base,
				p[0].boost, p[0].after_inner, p[0].after_outer,
				p[1].base, p[1].boost, p[1].after_inner,
				p[1].after_outer, ok ? "PASS" : "FAIL");
			mutex_destroy(&s.mutex);
			if (!ok)
				return -EINVAL;
		}
	return 0;
}
#else
static int chains(void)
{
	return check("chain_config_skipped", true);
}
#endif

#ifdef CONFIG_RWLOCK_PI
static int gate_cases(void)
{
	int cpu, write, m;
	for (cpu = 0; cpu < (num_online_cpus() > 1 ? 2 : 1); cpu++)
		for (write = 0; write < 2; write++)
			for (m = 0; m < 2; m++) {
				struct test_lock l;
				struct worker h = { .lock = &l,
						    .high = true,
						    .write = write },
					      medium = { .medium = true };
				int ret, base, held, after;
				bool ok;
				u64 release;
				init_lock(&l);
				ret = spawn(&h, cpu);
				if (ret)
					return ret;
				if (m) {
					ret = spawn(&medium, 0);
					if (ret)
						return ret;
				}
				base = READ_ONCE(current->prio);
				take(&l, true);
				complete(&h.start);
				ok = wait_blocked(h.task);
				held = READ_ONCE(current->prio);
				if (m)
					complete(&medium.start);
				busy(2000000);
				release = ktime_get_ns();
				drop(&l, true);
				after = READ_ONCE(current->prio);
				wait_for_completion(&h.done);
				if (m)
					wait_for_completion(&medium.done);
				stop(&h);
				if (m)
					stop(&medium);
				ok &= held == 49 && after == base &&
				      h.acquired >= release &&
				      (!m || medium.request >= release);
				pr_info("RLP_GATE,%d,%d,%d,%d/%d/%d,%s\n", cpu,
					write, m, base, held, after,
					ok ? "PASS" : "FAIL");
				if (!ok)
					return -EINVAL;
			}
	{
		struct test_lock l;
		struct worker w = { .lock = &l, .write = true, .high = true };
		int base = READ_ONCE(current->prio), ret;
		bool ok;
		init_lock(&l);
		ret = spawn(&w, num_online_cpus() > 1 ? 1 : 0);
		if (ret)
			return ret;
		take(&l, false);
		complete(&w.start);
		ok = wait_blocked(w.task) &&
		     !READ_ONCE(w.task->pi_blocked_on) &&
		     READ_ONCE(current->prio) == base;
		drop(&l, false);
		wait_for_completion(&w.done);
		stop(&w);
		return check("drain_no_reader_donation", ok);
	}
}
static rwlock_pi_t irq_negative_lock;
static struct completion irq_negative_done;
static int irq_negative_r, irq_negative_w;
static enum hrtimer_restart irq_try_callback(struct hrtimer *timer)
{
	irq_negative_r = pi_read_trylock(&irq_negative_lock);
	irq_negative_w = pi_write_trylock(&irq_negative_lock);
	complete(&irq_negative_done);
	return HRTIMER_NORESTART;
}
static int misuse(void)
{
	rwlock_pi_t l;
	unsigned long flags;
	int r, w;
	rwlock_init_pi(&l);
	pr_info("RLP_NEGATIVE_BEGIN,%d\n", negative);
	switch (negative) {
	case 1:
		preempt_disable();
		r = pi_read_trylock(&l);
		w = pi_write_trylock(&l);
		preempt_enable();
		return check("atomic_try_no_effect",
			     !r && !w &&
				     atomic_read(&l.readers) ==
					     (-2147483647 - 1) &&
				     ((unsigned long)READ_ONCE(l.gate.owner) ==
				      RT_MUTEX_PI_TAG));
	case 2:
		preempt_disable();
		pi_read_lock(&l);
		break;
	case 3:
		local_irq_save(flags);
		pi_write_lock(&l);
		local_irq_restore(flags);
		break;
	case 4:
		set_current_state(TASK_INTERRUPTIBLE);
		pi_read_lock(&l);
		break;
	case 5:
		pi_read_lock(&l);
		pi_read_lock(&l);
		break;
	case 6:
		pi_write_unlock(&l);
		break;
	case 7:
		local_irq_save(flags);
		r = pi_read_trylock(&l);
		w = pi_write_trylock(&l);
		local_irq_restore(flags);
		return check("irqoff_try_no_effect",
			     !r && !w &&
				     atomic_read(&l.readers) ==
					     (-2147483647 - 1));
	case 8:
		set_current_state(TASK_INTERRUPTIBLE);
		r = pi_read_trylock(&l);
		w = pi_write_trylock(&l);
		__set_current_state(TASK_RUNNING);
		return check("state_try_no_effect",
			     !r && !w &&
				     atomic_read(&l.readers) ==
					     (-2147483647 - 1));
	case 9: {
		struct hrtimer timer;
		rwlock_init_pi(&irq_negative_lock);
		init_completion(&irq_negative_done);
		hrtimer_init(&timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		timer.function = irq_try_callback;
		hrtimer_start(&timer, ns_to_ktime(1000000), HRTIMER_MODE_REL);
		wait_for_completion(&irq_negative_done);
		hrtimer_cancel(&timer);
		return check("hardirq_try_no_effect",
			     !irq_negative_r && !irq_negative_w &&
				     atomic_read(&irq_negative_lock.readers) ==
					     (-2147483647 - 1));
	}
	case 10:
		local_bh_disable();
		r = pi_read_trylock(&l);
		w = pi_write_trylock(&l);
		local_bh_enable();
		return check("bh_disabled_try_no_effect",
			     !r && !w &&
				     atomic_read(&l.readers) ==
					     (-2147483647 - 1));
	default:
		return -EINVAL;
	}
	return check("failstop_not_reached", false);
}
#else
static int gate_cases(void)
{
	return 0;
}
static int misuse(void)
{
	return -EINVAL;
}
#endif
static int __init test_init(void)
{
	cpumask_var_t saved;
	int ret;
	if (!alloc_cpumask_var(&saved, GFP_KERNEL))
		return -ENOMEM;
	cpumask_copy(saved, current->cpus_ptr);
	set_cpus_allowed_ptr(current, cpumask_of(0));
	pr_info("RLP_CONFIG,%d,%d,%d,sizes=%zu/%zu/%zu,file=%zu,inode=%zu,mm=%zu\n",
		IS_ENABLED(CONFIG_MUTEX_PI), IS_ENABLED(CONFIG_RWSEM_PI),
		IS_ENABLED(CONFIG_RWLOCK_PI), sizeof(rwlock_t),
		sizeof(rwlock_pi_t), sizeof(struct rw_semaphore),
		sizeof(struct file), sizeof(struct inode),
		sizeof(struct mm_struct));
	switch (suite) {
	case 0:
		ret = functional();
		if (!ret && IS_ENABLED(CONFIG_RWLOCK_PI) && kind)
			ret = gate_cases();
		break;
	case 1:
		ret = chains();
		break;
	case 2:
		ret = pressure_or_load(true);
		break;
	case 3:
		ret = uncontended();
		if (!ret)
			ret = pressure_or_load(false);
		break;
	case 4:
		ret = inversion();
		break;
	case 5:
		ret = misuse();
		break;
	case 6:
		ret = 0;
		break;
	default:
		ret = -EINVAL;
	}
	set_cpus_allowed_ptr(current, saved);
	free_cpumask_var(saved);
	pr_info("RLP_SUITE,%d,%s\n", suite, ret ? "FAIL" : "PASS");
	return ret;
}
static void __exit test_exit(void)
{
}
module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
