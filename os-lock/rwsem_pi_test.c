// SPDX-License-Identifier: GPL-2.0-only
/* Native rwsem baseline and independent serial PI prototype. */
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/rwsem.h>
#include <linux/rwsem_pi_serial.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>

#define WAIT_TIME (2 * HZ)

static DECLARE_RWSEM(static_sem);

struct peer {
	struct rw_semaphore *sem;
	struct rwsem_pi_serial *serial;
	struct task_struct *task;
	struct completion started, finished, release;
	bool write, fifo, probe, release_non_owner, medium;
	int state, ret, read_try, write_try, cpu, prio;
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
		if (p->serial) {
			p->read_try = rwsem_pi_serial_down_read_trylock(p->serial);
			if (p->read_try)
				rwsem_pi_serial_up_read(p->serial);
			p->write_try = rwsem_pi_serial_down_write_trylock(p->serial);
			if (p->write_try)
				rwsem_pi_serial_up_write(p->serial);
		} else {
			p->read_try = down_read_trylock(p->sem);
			if (p->read_try)
				up_read(p->sem);
			p->write_try = down_write_trylock(p->sem);
			if (p->write_try)
				up_write(p->sem);
		}
		complete(&p->finished);
		return 0;
	}
	if (p->release_non_owner) {
		up_read_non_owner(p->sem);
		complete(&p->finished);
		return 0;
	}

	p->request_ns = ktime_get_ns();
	if (p->serial) {
		if (p->write)
			rwsem_pi_serial_down_write(p->serial);
		else
			rwsem_pi_serial_down_read(p->serial);
	} else if (p->write) {
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
		if (p->serial) {
			if (p->write)
				rwsem_pi_serial_up_write(p->serial);
			else
				rwsem_pi_serial_up_read(p->serial);
		} else if (p->write)
			up_write(p->sem);
		else
			up_read(p->sem);
	}
	return 0;
}

static int start_peer(struct peer *p, int cpu)
{
	struct task_struct *task;

	init_completion(&p->started);
	init_completion(&p->finished);
	init_completion(&p->release);
	task = kthread_create(peer_run, p, "rwsem_base_peer");
	if (IS_ERR(task))
		return PTR_ERR(task);
	get_task_struct(task);
	p->task = task;
	kthread_bind(task, cpu);
	wake_up_process(task);
	return 0;
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

static int check_serial_trylock(int cpu)
{
	struct rwsem_pi_serial sem;
	struct peer w = { .serial = &sem, .probe = true };
	struct peer r = { .serial = &sem, .probe = true };
	bool pass;

	init_rwsem_pi_serial(&sem);
	if (!rwsem_pi_serial_down_read_trylock(&sem))
		return result("serial_trylock", false);
	rwsem_pi_serial_up_read(&sem);
	if (!rwsem_pi_serial_down_write_trylock(&sem))
		return result("serial_trylock", false);
	pass = !start_peer(&w, cpu) &&
		wait_for_completion_timeout(&w.finished, WAIT_TIME) &&
		!w.read_try && !w.write_try;
	rwsem_pi_serial_downgrade_write(&sem);
	pass = pass && !start_peer(&r, cpu) &&
		wait_for_completion_timeout(&r.finished, WAIT_TIME) &&
		!r.read_try && !r.write_try;
	rwsem_pi_serial_up_read(&sem);
	stop_peer(&w);
	stop_peer(&r);
	if (rwsem_pi_serial_down_write_trylock(&sem))
		rwsem_pi_serial_up_write(&sem);
	else
		pass = false;
	rt_mutex_destroy(&sem.lock);
	return result("serial_trylock", pass);
}

static int check_serial_pi(int cpu, bool owner_write, bool waiter_write,
			   const char *name)
{
	struct rwsem_pi_serial sem;
	struct peer h = { .serial = &sem, .write = waiter_write, .fifo = true,
			  .state = TASK_UNINTERRUPTIBLE };
	int owner_cpu = task_cpu(current);
	int before, held, after;
	u64 unlock_ns;
	bool blocked, pass;

	init_rwsem_pi_serial(&sem);
	if (owner_write)
		rwsem_pi_serial_down_write(&sem);
	else
		rwsem_pi_serial_down_read(&sem);
	before = READ_ONCE(current->prio);
	blocked = !start_peer(&h, cpu) && blocked_on_sem(&h) &&
		READ_ONCE(h.task->pi_blocked_on);
	held = READ_ONCE(current->prio);
	unlock_ns = ktime_get_ns();
	if (owner_write)
		rwsem_pi_serial_up_write(&sem);
	else
		rwsem_pi_serial_up_read(&sem);
	after = READ_ONCE(current->prio);
	pass = blocked && wait_for_completion_timeout(&h.finished, WAIT_TIME);
	stop_peer(&h);
	pass = pass && h.cpu == cpu && h.prio < before && held == h.prio &&
		after == before && h.request_ns <= unlock_ns &&
		h.acquire_ns >= unlock_ns && !rt_mutex_is_locked(&sem.lock);
	pr_info("RWT_SERIAL_PI,%s,cpus=%d/%d,H=%d,L=%d/%d/%d,blocked=%d,%s\n",
		name, owner_cpu, h.cpu, h.prio, before, held, after, blocked,
		pass ? "PASS" : "FAIL");
	rt_mutex_destroy(&sem.lock);
	return pass ? 0 : -EINVAL;
}

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
	if (!err)
		err = check_serial_trylock(cpu);
	if (!err)
		err = check_serial_pi(cpu, false, false, "reader_vs_reader");
	for (round = 0; round < 6 && !err; round++) {
		int waiter_cpu = round % 2 ? cpu : first;

		err = check_serial_pi(waiter_cpu, true, false, "reader_vs_writer");
		if (!err)
			err = check_serial_pi(waiter_cpu, false, true, "writer_vs_reader");
	}
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
	set_cpus_allowed_ptr(current, original);
	free_cpumask_var(original);
	return err;
}

static void __exit rwsem_pi_test_exit(void) { }
module_init(rwsem_pi_test_init);
module_exit(rwsem_pi_test_exit);
MODULE_LICENSE("GPL");
