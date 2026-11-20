// SPDX-License-Identifier: GPL-2.0-only
/* Native and opt-in rwsem tests through the public API. */
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/sched/cputime.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>

#define WAIT_TIME (2 * HZ)

static DECLARE_RWSEM(static_sem);
static DECLARE_RWSEM_PI(static_pi_sem);

struct peer {
	struct rw_semaphore *sem;
	struct mutex *mixed_mutex;
	struct task_struct *task;
	struct completion started, finished, release;
	bool write, fifo, probe, release_non_owner, medium;
	int state, ret, read_try, write_try, cpu, prio, tid;
	u64 request_ns, acquire_ns, release_ns;
};

static void busy_ns(u64 duration)
{
	u64 start = ktime_get_ns();

	while (ktime_get_ns() - start < duration)
		cpu_relax();
}

static int peer_run(void *arg)
{
	struct peer *p = arg;

	if (p->medium)
		sched_set_fifo_low(current);
	else if (p->fifo)
		sched_set_fifo(current);
	if (p->state != TASK_UNINTERRUPTIBLE) {
		allow_signal(SIGUSR1);
		allow_signal(SIGKILL);
	}
	p->cpu = task_cpu(current);
	p->prio = READ_ONCE(current->prio);
	p->tid = task_pid_nr(current);
	/* The controller may test the same readiness more than once. */
	complete_all(&p->started);
	if (p->medium) {
		wait_for_completion(&p->release);
		p->request_ns = ktime_get_ns();
		busy_ns(12000000);
		p->release_ns = ktime_get_ns();
		complete(&p->finished);
		return 0;
	}
	if (p->probe) {
		p->read_try = down_read_trylock(p->sem);
		if (p->read_try)
			up_read(p->sem);
		p->write_try = down_write_trylock(p->sem);
		if (p->write_try)
			up_write(p->sem);
		complete(&p->finished);
		return 0;
	}
	if (p->release_non_owner) {
		up_read_non_owner(p->sem);
		complete(&p->finished);
		return 0;
	}

	p->request_ns = ktime_get_ns();
	if (p->mixed_mutex)
		mutex_lock(p->mixed_mutex);
	else if (p->write) {
		if (p->state == TASK_KILLABLE)
			p->ret = down_write_killable(p->sem);
		else
			down_write(p->sem);
	} else if (p->state == TASK_KILLABLE) {
		p->ret = down_read_killable(p->sem);
	} else if (p->state == TASK_INTERRUPTIBLE) {
		p->ret = down_read_interruptible(p->sem);
	} else {
		down_read(p->sem);
	}
	p->acquire_ns = ktime_get_ns();
	complete(&p->finished);
	if (!p->ret) {
		wait_for_completion(&p->release);
		p->release_ns = ktime_get_ns();
		if (p->mixed_mutex)
			mutex_unlock(p->mixed_mutex);
		else if (p->write)
			up_write(p->sem);
		else
			up_read(p->sem);
	}
	return 0;
}

static int start_worker(struct peer *p, int cpu, int (*fn)(void *))
{
	struct task_struct *task;

	init_completion(&p->started);
	init_completion(&p->finished);
	init_completion(&p->release);
	task = kthread_create(fn, p, "rwsem_test_peer");
	if (IS_ERR(task))
		return PTR_ERR(task);
	get_task_struct(task);
	p->task = task;
	kthread_bind(task, cpu);
	wake_up_process(task);
	return 0;
}

static int start_peer(struct peer *p, int cpu)
{
	return start_worker(p, cpu, peer_run);
}

static void stop_peer(struct peer *p)
{
	if (!p->task)
		return;
	complete(&p->release);
	kthread_stop(p->task);
	put_task_struct(p->task);
}

static bool blocked_on_sem(struct peer *p)
{
	unsigned long until = jiffies + WAIT_TIME;

	if (!wait_for_completion_timeout(&p->started, WAIT_TIME))
		return false;
	do {
		if (completion_done(&p->finished))
			return false;
		if (READ_ONCE(p->task->state) == p->state &&
		    !smp_load_acquire(&p->task->on_cpu))
			return !completion_done(&p->finished);
		usleep_range(100, 200);
	} while (time_before(jiffies, until));
	pr_err("RWT_BLOCK_TIMEOUT,state=%ld,expected=%d,on_cpu=%d\n",
	       READ_ONCE(p->task->state), p->state, READ_ONCE(p->task->on_cpu));
	return false;
}

static int result(const char *name, bool pass)
{
	pr_info("RWT_CASE,%s,%s\n", name, pass ? "PASS" : "FAIL");
	return pass ? 0 : -EINVAL;
}

static int check_trylock(int cpu)
{
	struct peer w = { .sem = &static_sem, .probe = true };
	struct peer r = { .sem = &static_sem, .probe = true };
	bool pass;

	if (!down_read_trylock(&static_sem))
		return result("trylock", false);
	up_read(&static_sem);
	if (!down_write_trylock(&static_sem))
		return result("trylock", false);
	pass = !start_peer(&w, cpu) &&
		wait_for_completion_timeout(&w.finished, WAIT_TIME) &&
		!w.read_try && !w.write_try;
	up_write(&static_sem);
	stop_peer(&w);
	if (!pass)
		return result("trylock", false);

	down_read(&static_sem);
	pass = !start_peer(&r, cpu) &&
		wait_for_completion_timeout(&r.finished, WAIT_TIME) &&
		r.read_try == 1 && !r.write_try;
	up_read(&static_sem);
	stop_peer(&r);
	return result("trylock", pass && !rwsem_is_locked(&static_sem));
}

static int check_readers(int cpu)
{
	struct rw_semaphore sem;
	struct peer r = { .sem = &sem, .state = TASK_UNINTERRUPTIBLE };
	struct peer w = { .sem = &sem, .state = TASK_UNINTERRUPTIBLE, .write = true,
			  .fifo = true };
	int before, held, after, remote_held = 0;
	bool pass;

	init_rwsem(&sem);
	down_read(&sem);
	before = READ_ONCE(current->prio);
	pass = !start_peer(&r, cpu) &&
		wait_for_completion_timeout(&r.finished, WAIT_TIME) &&
		!start_peer(&w, cpu) && blocked_on_sem(&w);
	held = READ_ONCE(current->prio);
	if (pass)
		remote_held = READ_ONCE(r.task->prio);
	up_read(&sem);
	after = READ_ONCE(current->prio);
	/* The writer must still be queued after only the first reader exits. */
	if (pass)
		pass = blocked_on_sem(&w);
	stop_peer(&r);
	if (pass)
		pass = wait_for_completion_timeout(&w.finished, WAIT_TIME);
	stop_peer(&w);
	if (pass)
		pass = w.acquire_ns >= r.release_ns && w.prio < before;
	pr_info("RWT_PRIO,writer_vs_two_readers,H=%d,L=%d/%d/%d,R=%d/%d,cpus=%d/%d\n",
		w.prio, before, held, after, r.prio, remote_held, task_cpu(current), r.cpu);
	if (!pass)
		pr_err("RWT_READERS,reader_release_ns=%llu,writer_acquire_ns=%llu\n",
		       r.release_ns, w.acquire_ns);
	return result("parallel_readers", pass && held == before && after == before &&
		      remote_held == r.prio && r.cpu == cpu && !rwsem_is_locked(&sem));
}

static int check_writers(int cpu)
{
	struct rw_semaphore sem;
	struct peer a = { .sem = &sem, .write = true, .state = TASK_UNINTERRUPTIBLE };
	struct peer b = { .sem = &sem, .write = true, .state = TASK_UNINTERRUPTIBLE };
	bool pass;

	init_rwsem(&sem);
	down_write(&sem);
	pass = !start_peer(&a, cpu) && blocked_on_sem(&a) &&
		!start_peer(&b, cpu) && blocked_on_sem(&b);
	downgrade_write(&sem);
	if (pass)
		pass = blocked_on_sem(&a) && blocked_on_sem(&b);
	up_read(&sem);
	/* Do not assume the writer selected first. Allow either to leave. */
	if (pass) {
		complete(&a.release);
		complete(&b.release);
		pass = wait_for_completion_timeout(&a.finished, WAIT_TIME) &&
		       wait_for_completion_timeout(&b.finished, WAIT_TIME);
	}
	stop_peer(&a);
	stop_peer(&b);
	return result("downgrade_and_writers", pass &&
		      (a.release_ns <= b.acquire_ns || b.release_ns <= a.acquire_ns) &&
		      !rwsem_is_locked(&sem));
}

static int check_signals(int cpu, bool write, int state, const char *name)
{
	struct rw_semaphore sem;
	struct peer p = { .sem = &sem, .write = write, .state = state };
	bool pass;

	init_rwsem(&sem);
	down_write(&sem);
	pass = !start_peer(&p, cpu) && blocked_on_sem(&p) &&
		!send_sig(SIGUSR1, p.task, 1);
	if (pass && state == TASK_KILLABLE) {
		pass = !wait_for_completion_timeout(&p.finished, msecs_to_jiffies(30)) &&
		       blocked_on_sem(&p) && !send_sig(SIGKILL, p.task, 1);
	}
	if (pass)
		pass = wait_for_completion_timeout(&p.finished, WAIT_TIME) && p.ret == -EINTR;
	up_write(&sem);
	stop_peer(&p);
	if (pass && down_write_trylock(&sem))
		up_write(&sem);
	else
		pass = false;
	return result(name, pass && !rwsem_is_locked(&sem));
}

static int check_non_owner(int cpu)
{
	struct rw_semaphore sem;
	struct peer w = { .sem = &sem, .write = true, .state = TASK_UNINTERRUPTIBLE };
	struct peer release = { .sem = &sem, .release_non_owner = true };
	bool pass;

	init_rwsem(&sem);
	down_read_non_owner(&sem);
	pass = !start_peer(&w, cpu) && blocked_on_sem(&w);
	if (!pass || start_peer(&release, cpu)) {
		up_read_non_owner(&sem);
		pass = false;
	} else {
		pass = wait_for_completion_timeout(&release.finished, WAIT_TIME) &&
		       wait_for_completion_timeout(&w.finished, WAIT_TIME);
	}
	stop_peer(&release);
	stop_peer(&w);
	return result("non_owner", pass && !rwsem_is_locked(&sem));
}

static int check_no_pi(int cpu, bool write, const char *name)
{
	struct rw_semaphore sem;
	struct peer h = { .sem = &sem, .write = !write, .fifo = true,
			  .state = TASK_UNINTERRUPTIBLE };
	int before, held, after;
	u64 release_ns;
	bool pass;

	init_rwsem(&sem);
	if (write)
		down_write(&sem);
	else
		down_read(&sem);
	before = READ_ONCE(current->prio);
	pass = !start_peer(&h, cpu) && blocked_on_sem(&h);
	held = READ_ONCE(current->prio);
	/* Deliberately sleep while holding the semaphore; this is not CPU work. */
	msleep(10);
	pass &= !h.task || !completion_done(&h.finished);
	release_ns = ktime_get_ns();
	if (write)
		up_write(&sem);
	else
		up_read(&sem);
	after = READ_ONCE(current->prio);
	if (pass)
		pass = wait_for_completion_timeout(&h.finished, WAIT_TIME);
	stop_peer(&h);
	pr_info("RWT_WAIT,%s,H=%d,L=%d/%d/%d,wait_ns=%llu\n", name, h.prio,
		before, held, after, h.acquire_ns - h.request_ns);
	return result(name, pass && h.prio < before && held == before && after == before &&
		      h.acquire_ns >= release_ns && !rwsem_is_locked(&sem));
}

static int check_interference(int cpu, bool write, bool interfere, int round)
{
	struct rw_semaphore sem;
	struct peer h = { .sem = &sem, .write = !write, .fifo = true,
			  .state = TASK_UNINTERRUPTIBLE };
	struct peer m = { .state = TASK_UNINTERRUPTIBLE, .medium = true };
	int before, held, after;
	u64 owner_work, owner_release;
	bool pass = true;

	if (interfere) {
		if (start_peer(&m, cpu))
			return -ENOMEM;
		pass = wait_for_completion_timeout(&m.started, WAIT_TIME);
	}
	init_rwsem(&sem);
	if (write)
		down_write(&sem);
	else
		down_read(&sem);
	before = READ_ONCE(current->prio);
	pass = pass && !start_peer(&h, cpu) && blocked_on_sem(&h);
	held = READ_ONCE(current->prio);
	/* H is asleep on sem; waking FIFO M here preempts this runnable owner. */
	if (pass && interfere)
		complete(&m.release);
	owner_work = ktime_get_ns();
	busy_ns(2000000);
	owner_release = ktime_get_ns();
	if (write)
		up_write(&sem);
	else
		up_read(&sem);
	after = READ_ONCE(current->prio);
	if (pass)
		pass = wait_for_completion_timeout(&h.finished, WAIT_TIME);
	stop_peer(&h);
	stop_peer(&m);
	pass = pass && held == before && after == before && h.prio < before &&
	       h.request_ns <= owner_work && owner_release <= h.acquire_ns;
	if (interfere)
		pass = pass && h.prio < m.prio && m.prio < before &&
		       h.request_ns <= m.request_ns && m.release_ns <= owner_work;
	pr_info("RWT_INTERFERENCE,%s,%d,%d,H=%d,M=%d,L=%d/%d/%d,wait_ns=%llu,medium_ns=%llu,%s\n",
		write ? "reader_vs_writer" : "writer_vs_reader", round, interfere,
		h.prio, m.prio, before, held, after, h.acquire_ns - h.request_ns,
		m.release_ns - m.request_ns, pass ? "PASS" : "FAIL");
	return pass ? 0 : -EINVAL;
}

static bool real_pi_edge(struct peer *waiter, struct task_struct *owner)
{
	unsigned long flags;
	bool donated;

	if (!blocked_on_sem(waiter) || !READ_ONCE(waiter->task->pi_blocked_on))
		return false;
	raw_spin_lock_irqsave(&owner->pi_lock, flags);
	donated = owner->pi_top_task == waiter->task;
	raw_spin_unlock_irqrestore(&owner->pi_lock, flags);
	return donated;
}

#ifdef CONFIG_RWSEM_PI
static struct task_struct *gate_owner(struct rw_semaphore *sem)
{
	/* Mask both the gate's WAITERS bit and the backend tag. */
	unsigned long word = (unsigned long)READ_ONCE(sem->pi.gate.owner);

	return (struct task_struct *)(word & ~(RT_MUTEX_PI_TAG | 1UL));
}

static int check_pi_drain(int cpu)
{
	struct rw_semaphore sem;
	struct peer r = { .sem = &sem, .state = TASK_UNINTERRUPTIBLE };
	struct peer w = { .sem = &sem, .state = TASK_UNINTERRUPTIBLE,
			  .write = true, .fifo = true };
	struct peer newcomer = { .sem = &sem, .state = TASK_UNINTERRUPTIBLE };
	int base, reader_prio, owner_prio;
	bool parallel, drain, gate_wait, last, pass;

	init_rwsem_pi(&sem);
	down_read(&sem);
	base = READ_ONCE(current->prio);
	parallel = !start_peer(&r, cpu) &&
		   wait_for_completion_timeout(&r.finished, WAIT_TIME) &&
		   atomic_read(&sem.pi.readers) == RWSEM_PI_READER_BIAS + 2;
	drain = parallel && !start_peer(&w, cpu) && blocked_on_sem(&w) &&
		atomic_read(&sem.pi.readers) == 2 &&
		gate_owner(&sem) == w.task && !READ_ONCE(w.task->pi_blocked_on);
	reader_prio = parallel ? READ_ONCE(r.task->prio) : -1;
	owner_prio = READ_ONCE(current->prio);
	gate_wait = drain && !start_peer(&newcomer, cpu) && real_pi_edge(&newcomer, w.task);
	up_read(&sem);
	last = gate_wait && blocked_on_sem(&w) && atomic_read(&sem.pi.readers) == 1 &&
	       !completion_done(&newcomer.finished);
	stop_peer(&r);
	pass = last && wait_for_completion_timeout(&w.finished, WAIT_TIME) &&
	       atomic_read(&sem.pi.readers) == RWSEM_PI_WRITER_BIAS &&
	       !completion_done(&newcomer.finished);
	stop_peer(&w);
	pass = pass && wait_for_completion_timeout(&newcomer.finished, WAIT_TIME);
	stop_peer(&newcomer);
	pass = pass && w.acquire_ns >= r.release_ns && newcomer.acquire_ns >= w.release_ns &&
	       w.prio < base && owner_prio == base && reader_prio == r.prio &&
	       atomic_read(&sem.pi.readers) == RWSEM_PI_READER_BIAS &&
	       !rt_mutex_is_locked(&sem.pi.gate);
	pr_info("RWT_PI_DRAIN,parallel=%d,drain=%d,gate=%d,last=%d,H=%d,L=%d/%d,R=%d/%d,%s\n",
		parallel, drain, gate_wait, last, w.prio, base, owner_prio, r.prio,
		reader_prio, pass ? "PASS" : "FAIL");
	return pass ? 0 : -EINVAL;
}

static int check_pi_gate(int cpu, bool write, int state, const char *name)
{
	struct rw_semaphore sem;
	struct peer h = { .sem = &sem, .write = write, .fifo = true, .state = state };
	bool cancel = state != TASK_UNINTERRUPTIBLE;
	bool blocked, pass, ignored_usr1 = false, clean = false;
	unsigned long flags;
	int base, held, after;
	u64 release;

	init_rwsem_pi(&sem);
	down_write(&sem);
	base = READ_ONCE(current->prio);
	blocked = !start_peer(&h, cpu) && real_pi_edge(&h, current);
	held = READ_ONCE(current->prio);
	pass = blocked;
	if (pass && cancel) {
		pass = !send_sig(SIGUSR1, h.task, 1);
		if (pass && state == TASK_KILLABLE) {
			ignored_usr1 = !wait_for_completion_timeout(&h.finished, msecs_to_jiffies(30)) &&
				       real_pi_edge(&h, current) && READ_ONCE(current->prio) == held;
			pass = ignored_usr1 && !send_sig(SIGKILL, h.task, 1);
		}
		pass = pass && wait_for_completion_timeout(&h.finished, WAIT_TIME) && h.ret == -EINTR;
		raw_spin_lock_irqsave(&current->pi_lock, flags);
		clean = RB_EMPTY_ROOT(&current->pi_waiters.rb_root) && !current->pi_top_task;
		raw_spin_unlock_irqrestore(&current->pi_lock, flags);
		pass = pass && clean && !READ_ONCE(h.task->pi_blocked_on) &&
		       atomic_read(&sem.pi.readers) == RWSEM_PI_WRITER_BIAS &&
		       gate_owner(&sem) == current && READ_ONCE(current->prio) == base;
	}
	release = ktime_get_ns();
	up_write(&sem);
	after = READ_ONCE(current->prio);
	if (!cancel)
		pass = pass && wait_for_completion_timeout(&h.finished, WAIT_TIME) &&
		       h.acquire_ns >= release && !h.ret;
	stop_peer(&h);
	pass = pass && held == h.prio && held < base && after == base &&
	       atomic_read(&sem.pi.readers) == RWSEM_PI_READER_BIAS &&
	       !rt_mutex_is_locked(&sem.pi.gate);
	pr_info("RWT_PI_GATE,%s,cpus=%d/%d,H=%d,L=%d/%d/%d,blocked=%d,ret=%d,ignored_usr1=%d,clean=%d,%s\n",
		name, task_cpu(current), h.cpu, h.prio, base, held, after, blocked,
		h.ret, ignored_usr1, clean, pass ? "PASS" : "FAIL");
	return pass ? 0 : -EINVAL;
}

static int check_pi_drain_cancel(int cpu)
{
	struct rw_semaphore sem;
	struct peer w = { .sem = &sem, .state = TASK_KILLABLE, .write = true, .fifo = true };
	struct peer r = { .sem = &sem, .state = TASK_UNINTERRUPTIBLE };
	bool drain, ignored_usr1 = false, restored = false, pass;
	int base, held;

	init_rwsem_pi(&sem);
	down_read(&sem);
	base = READ_ONCE(current->prio);
	drain = !start_peer(&w, cpu) && blocked_on_sem(&w) &&
		atomic_read(&sem.pi.readers) == 1 && gate_owner(&sem) == w.task &&
		!READ_ONCE(w.task->pi_blocked_on);
	held = READ_ONCE(current->prio);
	if (drain && !send_sig(SIGUSR1, w.task, 1))
		ignored_usr1 = !wait_for_completion_timeout(&w.finished, msecs_to_jiffies(30)) &&
			       blocked_on_sem(&w) && atomic_read(&sem.pi.readers) == 1;
	if (ignored_usr1 && !send_sig(SIGKILL, w.task, 1))
		restored = wait_for_completion_timeout(&w.finished, WAIT_TIME) && w.ret == -EINTR &&
			   atomic_read(&sem.pi.readers) == RWSEM_PI_READER_BIAS + 1 &&
			   !rt_mutex_is_locked(&sem.pi.gate) && !READ_ONCE(w.task->pi_blocked_on);
	/* A new reader can enter before the original reader releases. */
	pass = restored && !start_peer(&r, cpu) &&
	       wait_for_completion_timeout(&r.finished, WAIT_TIME) &&
	       atomic_read(&sem.pi.readers) == RWSEM_PI_READER_BIAS + 2;
	up_read(&sem);
	stop_peer(&w);
	stop_peer(&r);
	pass = pass && w.prio < base && held == base &&
	       atomic_read(&sem.pi.readers) == RWSEM_PI_READER_BIAS;
	pr_info("RWT_PI_CANCEL,drain=%d,ignored_usr1=%d,restored=%d,L=%d/%d,H=%d,ret=%d,%s\n",
		drain, ignored_usr1, restored, base, held, w.prio, w.ret, pass ? "PASS" : "FAIL");
	return pass ? 0 : -EINVAL;
}

static int check_pi_downgrade(int cpu)
{
	struct rw_semaphore sem;
	struct peer w = { .sem = &sem, .state = TASK_UNINTERRUPTIBLE, .write = true };
	struct peer probe = { .sem = &sem, .probe = true };
	struct peer reader = { .sem = &sem, .probe = true };
	bool pass;

	init_rwsem_pi(&sem);
	if (!down_write_trylock(&sem))
		return result("rtbase_downgrade", false);
	pass = !start_peer(&probe, cpu) && wait_for_completion_timeout(&probe.finished, WAIT_TIME) &&
	       !probe.read_try && !probe.write_try;
	downgrade_write(&sem);
	pass = pass && atomic_read(&sem.pi.readers) == RWSEM_PI_READER_BIAS + 1 &&
	       !start_peer(&reader, cpu) && wait_for_completion_timeout(&reader.finished, WAIT_TIME) &&
	       reader.read_try && !reader.write_try &&
	       atomic_read(&sem.pi.readers) == RWSEM_PI_READER_BIAS + 1;
	pass = pass && !start_peer(&w, cpu) && blocked_on_sem(&w) &&
	       atomic_read(&sem.pi.readers) == 1 && gate_owner(&sem) == w.task;
	up_read(&sem);
	pass = pass && wait_for_completion_timeout(&w.finished, WAIT_TIME);
	stop_peer(&w);
	stop_peer(&reader);
	stop_peer(&probe);
	pass = pass && atomic_read(&sem.pi.readers) == RWSEM_PI_READER_BIAS &&
	       !rt_mutex_is_locked(&sem.pi.gate);
	return result("rtbase_downgrade", pass);
}

struct mixed_chain {
	struct mutex mutex;
	struct rw_semaphore rwsem;
	struct peer low;
	bool rwsem_at_root;
	int at_handoff, after_root, after_outer;
};

static int mixed_chain_low(void *arg)
{
	struct peer *low = arg;
	struct mixed_chain *s = container_of(low, struct mixed_chain, low);

	sched_set_fifo_low(current);
	low->prio = READ_ONCE(current->prio);
	low->cpu = task_cpu(current);
	if (s->rwsem_at_root)
		mutex_lock(&s->mutex);
	else
		down_write(&s->rwsem);
	complete_all(&low->started);
	if (s->rwsem_at_root)
		down_write(&s->rwsem);
	else
		mutex_lock(&s->mutex);
	s->at_handoff = READ_ONCE(current->prio);
	if (s->rwsem_at_root)
		up_write(&s->rwsem);
	else
		mutex_unlock(&s->mutex);
	s->after_root = READ_ONCE(current->prio);
	low->release_ns = ktime_get_ns();
	if (s->rwsem_at_root)
		mutex_unlock(&s->mutex);
	else
		up_write(&s->rwsem);
	s->after_outer = READ_ONCE(current->prio);
	complete(&low->finished);
	return 0;
}

static int check_mixed_chain(int cpu, bool rwsem_at_root)
{
	struct mixed_chain s = { .low.state = TASK_UNINTERRUPTIBLE, .rwsem_at_root = rwsem_at_root };
	struct peer h = { .fifo = true, .state = TASK_UNINTERRUPTIBLE };
	bool lx, hl, lx_again, pass;
	int base, middle, high, after;

	/* The two independent hierarchies use distinct lockdep classes. */
	if (rwsem_at_root) {
		mutex_init_pi(&s.mutex);
		init_rwsem_pi(&s.rwsem);
		h.mixed_mutex = &s.mutex;
		down_write(&s.rwsem);
	} else {
		mutex_init_pi(&s.mutex);
		init_rwsem_pi(&s.rwsem);
		h.sem = &s.rwsem;
		mutex_lock(&s.mutex);
	}
	base = READ_ONCE(current->prio);
	lx = !start_worker(&s.low, cpu, mixed_chain_low) && real_pi_edge(&s.low, current);
	middle = READ_ONCE(current->prio);
	hl = lx && !start_peer(&h, cpu) && real_pi_edge(&h, s.low.task);
	lx_again = hl && real_pi_edge(&s.low, current);
	high = READ_ONCE(current->prio);
	pass = lx_again && READ_ONCE(s.low.task->prio) == h.prio;
	if (rwsem_at_root)
		up_write(&s.rwsem);
	else
		mutex_unlock(&s.mutex);
	after = READ_ONCE(current->prio);
	pass = pass && wait_for_completion_timeout(&s.low.finished, WAIT_TIME) &&
	       wait_for_completion_timeout(&h.finished, WAIT_TIME);
	stop_peer(&h);
	stop_peer(&s.low);
	pass = pass && h.cpu == cpu && s.low.cpu == cpu && h.prio < s.low.prio &&
	       s.low.prio < base && middle == s.low.prio && high == h.prio && after == base &&
	       s.at_handoff == h.prio && s.after_root == h.prio && s.after_outer == s.low.prio &&
	       s.low.release_ns <= h.acquire_ns && !mutex_is_locked(&s.mutex) &&
	       !rt_mutex_is_locked(&s.rwsem.pi.gate);
	pr_info("RWT_MIXED_CHAIN,%d,cpus=%d/%d/%d,X=%d/%d/%d/%d,L=%d/%d/%d/%d,H=%d,edges=%d/%d/%d,%s\n",
		rwsem_at_root, task_cpu(current), s.low.cpu, h.cpu, base, middle, high, after,
		s.low.prio, s.at_handoff, s.after_root, s.after_outer, h.prio, lx, hl, lx_again,
		pass ? "PASS" : "FAIL");
	mutex_destroy(&s.mutex);
	return pass ? 0 : -EINVAL;
}

#endif /* CONFIG_RWSEM_PI */

static int suite;
static int readers = -1;
static bool pi_mode;
module_param(suite, int, 0444);
module_param(readers, int, 0444);
module_param(pi_mode, bool, 0444);
MODULE_PARM_DESC(suite, "0=functional, 1=pressure, 2=throughput, 3=interference, 4=mixed chain, 5=uncontended");
MODULE_PARM_DESC(pi_mode, "Use init_rwsem_pi in performance suites");
MODULE_PARM_DESC(readers, "Suite 2 reader count (-1 runs the full sequence)");

#define LOAD_WORKERS 8
#define LOAD_OPS 25000
#define PRESSURE_NS (20ULL * NSEC_PER_SEC)
struct comparison_load {
	struct rw_semaphore sem;
	struct completion start;
	atomic_t readers, writers;
	u64 start_ns;
};
struct comparison_worker {
	struct comparison_load *load;
	struct task_struct *task;
	struct completion done;
	bool write;
	int cpu, peak, bad;
	u64 ops, max_wait, finish_ns, cpu_ns;
};

static int comparison_worker(void *arg)
{
	struct comparison_worker *p = arg;
	struct comparison_load *s = p->load;
	bool pressure = suite == 1;
	u64 u0, k0, u1, k1, requested, acquired;

	wait_for_completion(&s->start);
	task_cputime_adjusted(current, &u0, &k0);
	while (!kthread_should_stop()) {
		requested = ktime_get_ns();
		if (pressure ? requested - s->start_ns >= PRESSURE_NS : p->ops >= LOAD_OPS)
			break;
		if (p->write)
			down_write(&s->sem);
		else
			down_read(&s->sem);
		acquired = ktime_get_ns();
		if (pressure) {
			if (p->write) {
				if (atomic_inc_return(&s->writers) != 1 || atomic_read(&s->readers))
					p->bad++;
			} else {
				p->peak = max(p->peak, atomic_inc_return(&s->readers));
				if (atomic_read(&s->writers))
					p->bad++;
			}
		}
		if (pressure && !(p->ops % 128))
			usleep_range(100, 200);
		else
			busy_ns(2500);
		if (p->write) {
			if (pressure)
				atomic_dec(&s->writers);
			up_write(&s->sem);
		} else {
			if (pressure)
				atomic_dec(&s->readers);
			up_read(&s->sem);
		}
		p->max_wait = max(p->max_wait, acquired - requested);
		p->ops++;
		cond_resched();
	}
	p->finish_ns = ktime_get_ns();
	task_cputime_adjusted(current, &u1, &k1);
	p->cpu_ns = u1 - u0 + k1 - k0;
	complete(&p->done);
	return 0;
}

static int comparison_load(int nr_readers)
{
	struct comparison_load s = { };
	struct comparison_worker workers[LOAD_WORKERS] = { };
	int i, n, cpu = cpumask_first(cpu_online_mask), peak = 0, errors = 0;
	u64 end = 0;

	init_completion(&s.start);
	if (pi_mode)
		init_rwsem_pi(&s.sem);
	else
		init_rwsem(&s.sem);
	for (i = 0; i < LOAD_WORKERS; i++) {
		struct comparison_worker *p = &workers[i];

		p->load = &s;
		p->write = i >= nr_readers;
		p->cpu = cpu;
		init_completion(&p->done);
		p->task = kthread_create(comparison_worker, p, "rwcompare/%d", i);
		if (IS_ERR(p->task))
			break;
		get_task_struct(p->task);
		kthread_bind(p->task, cpu);
		wake_up_process(p->task);
		cpu = cpumask_next(cpu, cpu_online_mask);
		if (cpu >= nr_cpu_ids)
			cpu = cpumask_first(cpu_online_mask);
	}
	n = i;
	s.start_ns = ktime_get_ns();
	complete_all(&s.start);
	for (i = 0; i < n; i++) {
		struct comparison_worker *p = &workers[i];

		if (n == LOAD_WORKERS)
			wait_for_completion(&p->done);
		kthread_stop(p->task);
		put_task_struct(p->task);
		end = max(end, p->finish_ns);
		peak = max(peak, p->peak);
		errors += p->bad + !p->ops;
		pr_info("RWC_WORK,%d,%d,%d,%d,%d,%llu,%llu,%llu,%d\n", pi_mode, nr_readers,
			i, p->cpu, p->write, p->ops, p->max_wait, p->cpu_ns, p->bad);
	}
	if (n != LOAD_WORKERS)
		return -ENOMEM;
	errors += atomic_read(&s.readers) != 0 || atomic_read(&s.writers) != 0;
	errors += suite == 1 && num_online_cpus() > 1 &&
		  peak < 2;
	pr_info("RWC_LOAD,%d,%d,%llu,%d,%d\n", pi_mode, nr_readers, end - s.start_ns, peak, errors);
	return errors ? -EINVAL : 0;
}

static int comparison_interference(int cpu, bool write, bool interfere, int round)
{
	struct rw_semaphore sem;
	struct peer h = { .write = !write, .fifo = true, .state = TASK_UNINTERRUPTIBLE };
	struct peer m = { .state = TASK_UNINTERRUPTIBLE, .medium = true };
	bool gate, drain;
	int base, boosted, after;
	u64 medium_ready, release;
	bool pass = true;

	if (pi_mode)
		init_rwsem_pi(&sem);
	else
		init_rwsem(&sem);
	h.sem = &sem;
	gate = rwsem_uses_pi(&sem) && write;
	drain = rwsem_uses_pi(&sem) && !write;
	if (interfere) {
		if (start_peer(&m, cpu))
			return -ENOMEM;
		pass = wait_for_completion_timeout(&m.started, WAIT_TIME);
	}
	if (write)
		down_write(&sem);
	else
		down_read(&sem);
	base = READ_ONCE(current->prio);
	pass = pass && !start_peer(&h, cpu) &&
	       (gate ? real_pi_edge(&h, current) : blocked_on_sem(&h));
#ifdef CONFIG_RWSEM_PI
	if (drain)
		pass = pass && atomic_read(&sem.pi.readers) == 1 &&
		       gate_owner(&sem) == h.task && !READ_ONCE(h.task->pi_blocked_on);
#endif
	boosted = READ_ONCE(current->prio);
	medium_ready = ktime_get_ns();
	if (interfere)
		complete(&m.release);
	busy_ns(2000000);
	release = ktime_get_ns();
	if (write)
		up_write(&sem);
	else
		up_read(&sem);
	after = READ_ONCE(current->prio);
	pass = pass && wait_for_completion_timeout(&h.finished, WAIT_TIME) &&
	       (!interfere || wait_for_completion_timeout(&m.finished, WAIT_TIME));
	stop_peer(&h);
	stop_peer(&m);
	pass = pass && h.prio < base && boosted == (gate ? h.prio : base) &&
	       after == base && h.request_ns <= release && release <= h.acquire_ns;
	if (interfere)
		pass = pass && h.prio < m.prio && m.prio < base &&
		       (gate ? h.acquire_ns <= m.request_ns : m.release_ns <= release);
	pr_info("RWC_LATENCY,%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%d,%d,%llu,%llu,%llu,%llu,%llu,%s\n",
		pi_mode, round, write, interfere, gate ? "WAIT_GATE" : (drain ? "DRAIN" : "NATIVE"),
		task_pid_nr(current), h.tid, m.tid, h.prio, base, boosted, after, h.request_ns, h.acquire_ns,
		medium_ready, m.request_ns, m.release_ns, pass ? "PASS" : "FAIL");
	return pass ? 0 : -EINVAL;
}

static int check_initializers(void)
{
	struct rw_semaphore tagged, plain;
	bool pass;

	init_rwsem_pi(&tagged);
	init_rwsem(&plain);
	pass = rwsem_uses_pi(&tagged) == IS_ENABLED(CONFIG_RWSEM_PI) &&
	       rwsem_uses_pi(&static_pi_sem) == IS_ENABLED(CONFIG_RWSEM_PI) &&
	       !rwsem_uses_pi(&plain) && !rwsem_uses_pi(&static_sem) &&
	       !rwsem_is_locked(&static_pi_sem) && !rwsem_is_contended(&static_pi_sem);
	down_write(&static_pi_sem);
	pass &= rwsem_is_locked(&static_pi_sem);
	downgrade_write(&static_pi_sem);
	up_read(&static_pi_sem);
	pass &= !rwsem_is_locked(&static_pi_sem) && !rwsem_is_contended(&static_pi_sem);
	if (!rwsem_uses_pi(&tagged)) {
		down_read_non_owner(&tagged);
		pass &= rwsem_is_locked(&tagged);
		up_read_non_owner(&tagged);
		pass &= !rwsem_is_locked(&tagged);
	}
	return result("initializers", pass);
}

static int check_nested(void)
{
	struct rw_semaphore sem[2];
	struct mutex nest;
	bool pass = true;
	int i;

	for (i = 0; i < ARRAY_SIZE(sem); i++)
		init_rwsem_pi(&sem[i]);
	down_read_nested(&sem[0], 0);
	if (!down_read_killable_nested(&sem[1], 1))
		up_read(&sem[1]);
	else
		pass = false;
	up_read(&sem[0]);
	down_write_nested(&sem[0], 0);
	if (!down_write_killable_nested(&sem[1], 1))
		up_write(&sem[1]);
	else
		pass = false;
	up_write(&sem[0]);
	mutex_init(&nest);
	mutex_lock(&nest);
	down_write_nest_lock(&sem[0], &nest);
	down_write_nest_lock(&sem[1], &nest);
	up_write(&sem[1]);
	up_write(&sem[0]);
	mutex_unlock(&nest);
	mutex_destroy(&nest);
	return result("nested", pass && !rwsem_is_locked(&sem[0]) && !rwsem_is_locked(&sem[1]));
}

static int uncontended(void)
{
	struct rw_semaphore sem;
	const int pairs = 100000;
	int round, write, i;
	unsigned long flags;
	u64 elapsed;

	if (pi_mode)
		init_rwsem_pi(&sem);
	else
		init_rwsem(&sem);
	for (round = 0; round <= 31; round++) {
		for (write = 0; write < 2; write++) {
			/* Do not disable preemption: down_*() is a sleeping API. */
			elapsed = ktime_get_ns();
			if (write) {
				for (i = 0; i < pairs; i++) {
					down_write(&sem);
					up_write(&sem);
				}
			} else {
				for (i = 0; i < pairs; i++) {
					down_read(&sem);
					up_read(&sem);
				}
			}
			elapsed = ktime_get_ns() - elapsed;
			if (round)
				pr_info("RWC_PAIR,%d,%d,%d,%d,%llu\n", pi_mode, round, write, pairs, elapsed);
		}
	}
	local_irq_save(flags);
	/* A task-context trylock must preserve its incoming interrupt state. */
	i = down_write_trylock(&sem);
	if (i)
		up_write(&sem);
	write = irqs_disabled();
	local_irq_restore(flags);
	return result("uncontended", i && write && !rwsem_is_locked(&sem));
}

#ifdef CONFIG_RWSEM_PI
static int check_pi_helpers(void)
{
	struct rw_semaphore sem;
	const struct { int count; bool locked, contended; } states[] = {
		{ RWSEM_PI_READER_BIAS, false, false },
		{ RWSEM_PI_READER_BIAS + 2, true, false },
		{ 2, true, true },
		{ 0, true, false },
		{ RWSEM_PI_WRITER_BIAS, true, true },
	};
	bool pass = true;
	int i;

	init_rwsem_pi(&sem);
	/* Private fixture: includes DRAIN(0), which is transient on real locks. */
	for (i = 0; i < ARRAY_SIZE(states); i++) {
		atomic_set(&sem.pi.readers, states[i].count);
		pass &= rwsem_is_locked(&sem) == states[i].locked &&
			rwsem_is_contended(&sem) == states[i].contended;
		pr_info("RWT_PI_STATE,%d,%d,%d,%d\n", i, states[i].count,
			rwsem_is_locked(&sem), rwsem_is_contended(&sem));
	}
	return result("pi_helpers", pass);
}

static int check_interruptible_reader_during_drain(int cpu)
{
	struct rw_semaphore sem;
	struct peer w = { .sem = &sem, .write = true, .state = TASK_KILLABLE };
	struct peer r = { .sem = &sem, .fifo = true, .state = TASK_INTERRUPTIBLE };
	bool drain, wait_gate, canceled = false, finished = false, pass;
	int base = READ_ONCE(current->prio);

	init_rwsem_pi(&sem);
	down_read(&sem);
	drain = !start_peer(&w, cpu) && blocked_on_sem(&w) &&
		gate_owner(&sem) == w.task && atomic_read(&sem.pi.readers) == 1 &&
		!READ_ONCE(w.task->pi_blocked_on);
	wait_gate = drain && !start_peer(&r, cpu) && real_pi_edge(&r, w.task) &&
		    READ_ONCE(w.task->prio) == r.prio && READ_ONCE(current->prio) == base;
	if (wait_gate && !send_sig(SIGUSR1, r.task, 1)) {
		canceled = wait_for_completion_timeout(&r.finished, WAIT_TIME) &&
			   r.ret == -EINTR && !READ_ONCE(r.task->pi_blocked_on) &&
			   !READ_ONCE(w.task->pi_top_task) && READ_ONCE(w.task->prio) == w.prio &&
			   gate_owner(&sem) == w.task && atomic_read(&sem.pi.readers) == 1 &&
			   !completion_done(&w.finished);
	}
	up_read(&sem);
	if (w.task)
		finished = wait_for_completion_timeout(&w.finished, WAIT_TIME) && !w.ret;
	stop_peer(&w);
	stop_peer(&r);
	pass = wait_gate && canceled && finished && !rwsem_is_locked(&sem) && !gate_owner(&sem);
	return result("interruptible_reader_during_drain", pass);
}
#endif

static int __init rwsem_pi_test_init(void)
{
	int first = cpumask_first(cpu_online_mask);
	int cpu = num_online_cpus() > 1 ? cpumask_next(first, cpu_online_mask) : first;
	cpumask_var_t original;
	int err, round, i;

	if (!alloc_cpumask_var(&original, GFP_KERNEL))
		return -ENOMEM;
	cpumask_copy(original, current->cpus_ptr);
	err = set_cpus_allowed_ptr(current, cpumask_of(first));
	if (err)
		goto restore;
	pr_info("RWT_CONFIG,%d,%d,%zu\n", IS_ENABLED(CONFIG_MUTEX_PI),
		IS_ENABLED(CONFIG_RWSEM_PI), sizeof(struct rw_semaphore));
	pr_info("RWT_SIZE,%zu,%zu,%zu,%zu\n", sizeof(struct rw_semaphore),
		sizeof(struct inode), sizeof(struct mm_struct), sizeof(struct file));
	switch (suite) {
	case 0:
		break;
	case 1:
		err = comparison_load(4);
		goto restore;
	case 2: {
		const int mixes[] = {8, 7, 4, 0};

		err = -EINVAL;
		for (i = 0; i < ARRAY_SIZE(mixes); i++) {
			if (readers != -1 && readers != mixes[i])
				continue;
			err = comparison_load(mixes[i]);
			if (err)
				break;
		}
		goto restore;
	}
	case 3:
		for (round = 1; round <= 12 && !err; round++) {
			for (i = 0; i < 4 && !err; i++) {
				int mode = (round + i) % 4;

				err = comparison_interference(first, mode / 2, mode % 2, round);
			}
		}
		goto restore;
	case 4:
#ifdef CONFIG_RWSEM_PI
		if (IS_ENABLED(CONFIG_MUTEX_PI)) {
			for (round = 0; round < 6 && !err; round++) {
				int target = round % 2 ? cpu : first;

				err = check_mixed_chain(target, false);
				if (!err)
					err = check_mixed_chain(target, true);
			}
			goto restore;
		}
#endif
		err = -EOPNOTSUPP;
		goto restore;
	case 5:
		err = uncontended();
		goto restore;
	default:
		err = -EINVAL;
		goto restore;
	}
	err = check_initializers();
	if (!err)
		err = check_nested();
#ifdef CONFIG_RWSEM_PI
	if (!err)
		err = check_pi_helpers();
	if (!err)
		err = check_pi_drain(cpu);
	for (round = 0; round < 2 && !err; round++) {
		int target = round ? cpu : first;

		err = check_pi_gate(target, false, TASK_UNINTERRUPTIBLE, "reader_vs_writer");
		if (!err)
			err = check_pi_gate(target, true, TASK_UNINTERRUPTIBLE, "writer_vs_writer");
	}
	if (!err)
		err = check_pi_gate(cpu, false, TASK_INTERRUPTIBLE, "read_interruptible");
	if (!err)
		err = check_pi_gate(cpu, false, TASK_KILLABLE, "read_killable");
	if (!err)
		err = check_pi_gate(cpu, true, TASK_KILLABLE, "write_killable");
	if (!err)
		err = check_pi_drain_cancel(cpu);
	if (!err)
		err = check_interruptible_reader_during_drain(cpu);
	if (!err)
		err = check_pi_downgrade(cpu);
#endif
	if (!err)
		err = check_trylock(cpu);
	if (!err)
		err = check_readers(cpu);
	if (!err)
		err = check_writers(cpu);
	if (!err)
		err = check_signals(cpu, false, TASK_INTERRUPTIBLE, "read_interruptible");
	if (!err)
		err = check_signals(cpu, false, TASK_KILLABLE, "read_killable");
	if (!err)
		err = check_signals(cpu, true, TASK_KILLABLE, "write_killable");
	if (!err)
		err = check_non_owner(cpu);
	if (!err)
		err = check_no_pi(cpu, true, "reader_vs_sleeping_writer");
	if (!err)
		err = check_no_pi(cpu, false, "writer_vs_sleeping_reader");
	for (round = 0; round < 6 && !err; round++) {
		for (i = 0; i < 4 && !err; i++) {
			int mode = (round + i) % 4;

			err = check_interference(first, mode / 2, mode % 2, round + 1);
		}
	}
restore:
	set_cpus_allowed_ptr(current, original);
	free_cpumask_var(original);
	return err;
}
static void __exit rwsem_pi_test_exit(void) { }
module_init(rwsem_pi_test_init);
module_exit(rwsem_pi_test_exit);
MODULE_LICENSE("GPL");
