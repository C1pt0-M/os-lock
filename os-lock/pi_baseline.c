// SPDX-License-Identifier: GPL-2.0-only
/*
 * Controlled single-lock PI baseline. Minimal check in the QEMU guest:
 *     timeout 30 /bin/pi_baseline both 1
 *
 * All workers use one CPU: L=FIFO/10, M=FIFO/20, H=FIFO/30.
 * L first owns the target mutex and waits at a setup gate. H opens L/M's
 * work gates immediately before requesting that mutex. M never takes it.
 * L burns 20 ms of thread CPU time; M burns 100 ms, without timed sleeps.
 * The controller uses a second CPU. PI-on/off is a mechanism control, not
 * evidence of a new kernel optimization. No chain or multi-reader coverage.
 * A 1-second idle gap before each trial replenishes the default RT budget;
 * this fixed baseline rejects non-default RT bandwidth settings.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum role { LOW, MEDIUM, HIGH, WORKERS };
static const int priorities[WORKERS] = { 10, 20, 30 };
static const uint64_t low_work_ns = UINT64_C(20000000);
static const uint64_t medium_work_ns = UINT64_C(100000000);

struct trial;
struct worker {
    struct trial *trial;
    enum role role;
    pid_t tid;
};

struct trial {
    pthread_mutex_t lock;
    sem_t ready[WORKERS];
    sem_t gate[WORKERS];
    struct worker workers[WORKERS];
    int worker_cpu;
    uint64_t low_locked;
    uint64_t low_started;
    uint64_t low_unlock_begin;
    uint64_t medium_started;
    uint64_t medium_finished;
    uint64_t high_requested;
    uint64_t high_acquired;
    uint64_t low_cpu_time;
    uint64_t medium_cpu_time;
};

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s (error %d)\n", operation,
                strerror(error), error);
        exit(EXIT_FAILURE);
    }
}

static void check_system(int result, const char *operation)
{
    if (result == -1) {
        perror(operation);
        exit(EXIT_FAILURE);
    }
}

static long read_rt_setting(const char *path)
{
    FILE *file = fopen(path, "r");
    long value;

    if (!file) {
        perror(path);
        exit(EXIT_FAILURE);
    }
    if (fscanf(file, "%ld", &value) != 1) {
        fprintf(stderr, "Cannot read RT bandwidth setting: %s\n", path);
        exit(EXIT_FAILURE);
    }
    fclose(file);
    return value;
}

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec ts;
    check_system(clock_gettime(clock, &ts), "clock_gettime");
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static struct timespec deadline(void)
{
    struct timespec ts;
    check_system(clock_gettime(CLOCK_REALTIME, &ts), "clock_gettime(deadline)");
    ts.tv_sec += 5;
    return ts;
}

static void wait_gate(sem_t *gate)
{
    struct timespec until = deadline();
    while (sem_timedwait(gate, &until) != 0) {
        if (errno != EINTR) {
            perror("sem_timedwait: setup did not complete within deadline");
            exit(EXIT_FAILURE);
        }
    }
}

static uint64_t burn_cpu(uint64_t duration)
{
    volatile uint64_t value = 1;
    uint64_t start = clock_ns(CLOCK_THREAD_CPUTIME_ID);

    for (;;) {
        for (int i = 0; i < 256; ++i)
            value = value * UINT64_C(6364136223846793005) + 1;
        uint64_t now = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        if (now < start) {
            fprintf(stderr, "Thread CPU clock moved backwards\n");
            exit(EXIT_FAILURE);
        }
        if (now - start >= duration)
            return now - start;
    }
}

static void configure_worker(struct worker *worker)
{
    cpu_set_t requested, actual;
    struct sched_param parameters = {
        .sched_priority = priorities[worker->role]
    };
    struct sched_param observed;
    int policy;

    CPU_ZERO(&requested);
    CPU_SET(worker->trial->worker_cpu, &requested);
    check_pthread(pthread_setaffinity_np(pthread_self(), sizeof(requested),
                                       &requested),
                  "pthread_setaffinity_np");
    check_pthread(pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual),
                  "pthread_getaffinity_np");
    if (CPU_COUNT(&actual) != 1 ||
        !CPU_ISSET(worker->trial->worker_cpu, &actual)) {
        fprintf(stderr, "Worker CPU affinity mismatch\n");
        exit(EXIT_FAILURE);
    }
    check_pthread(pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters),
                  "pthread_setschedparam(SCHED_FIFO)");
    check_pthread(pthread_getschedparam(pthread_self(), &policy, &observed),
                  "pthread_getschedparam");
    if (policy != SCHED_FIFO ||
        observed.sched_priority != parameters.sched_priority) {
        fprintf(stderr, "Worker scheduling policy/priority mismatch\n");
        exit(EXIT_FAILURE);
    }
    worker->tid = (pid_t)syscall(SYS_gettid);
    if (worker->tid <= 0) {
        fprintf(stderr, "Unable to read worker TID\n");
        exit(EXIT_FAILURE);
    }
}

static void *run_worker(void *argument)
{
    struct worker *worker = argument;
    struct trial *trial = worker->trial;
    configure_worker(worker);

    if (worker->role == LOW) {
        check_pthread(pthread_mutex_lock(&trial->lock), "L: mutex_lock");
        trial->low_locked = clock_ns(CLOCK_MONOTONIC);
    }
    check_system(sem_post(&trial->ready[worker->role]), "sem_post(ready)");
    wait_gate(&trial->gate[worker->role]);

    if (worker->role == LOW) {
        trial->low_started = clock_ns(CLOCK_MONOTONIC);
        trial->low_cpu_time = burn_cpu(low_work_ns);
        /* Timestamp before unlock: deboost/wakeup may preempt us inside it. */
        trial->low_unlock_begin = clock_ns(CLOCK_MONOTONIC);
        check_pthread(pthread_mutex_unlock(&trial->lock), "L: mutex_unlock");
    } else if (worker->role == MEDIUM) {
        trial->medium_started = clock_ns(CLOCK_MONOTONIC);
        trial->medium_cpu_time = burn_cpu(medium_work_ns);
        trial->medium_finished = clock_ns(CLOCK_MONOTONIC);
    } else {
        /* H is highest-priority on this CPU; lower-priority work cannot
         * normally run until H blocks. Release both gates here, not from
         * the controller, to avoid a cross-CPU gate-release race. */
        check_system(sem_post(&trial->gate[MEDIUM]), "H: sem_post(M)");
        check_system(sem_post(&trial->gate[LOW]), "H: sem_post(L)");
        trial->high_requested = clock_ns(CLOCK_MONOTONIC);
        check_pthread(pthread_mutex_lock(&trial->lock), "H: mutex_lock");
        trial->high_acquired = clock_ns(CLOCK_MONOTONIC);
        check_pthread(pthread_mutex_unlock(&trial->lock), "H: mutex_unlock");
    }
    return NULL;
}

static void check_order(const struct trial *trial, bool pi)
{
    bool timestamps_valid =
        trial->low_locked <= trial->high_requested &&
        trial->high_requested <= trial->low_started &&
        trial->high_requested <= trial->medium_started &&
        trial->low_started <= trial->low_unlock_begin &&
        trial->low_unlock_begin <= trial->high_acquired &&
        trial->medium_started <= trial->medium_finished;
    bool order_valid = pi
        ? trial->low_unlock_begin < trial->medium_finished
        : trial->medium_finished <= trial->low_started;

    if (!timestamps_valid || !order_valid) {
        fprintf(stderr,
                "INVALID trial (%s): unexpected timestamps/execution order; "
                "do not use it for performance comparison.\n"
                "L_locked=%" PRIu64 " H_requested=%" PRIu64
                " L_started=%" PRIu64 " L_unlock_begin=%" PRIu64
                " H_acquired=%" PRIu64 " M_started=%" PRIu64
                " M_finished=%" PRIu64 "\n",
                pi ? "pi" : "none", trial->low_locked, trial->high_requested,
                trial->low_started, trial->low_unlock_begin,
                trial->high_acquired, trial->medium_started,
                trial->medium_finished);
        exit(EXIT_FAILURE);
    }
}

static void run_trial(bool pi, int round, int worker_cpu, int controller_cpu)
{
    struct trial trial = { .worker_cpu = worker_cpu };
    pthread_mutexattr_t attributes;
    pthread_t threads[WORKERS];
    int protocol = pi ? PTHREAD_PRIO_INHERIT : PTHREAD_PRIO_NONE;
    int observed_protocol;
    struct timespec idle = { .tv_sec = 1 };
    struct timespec remaining;

    /* Outside H's measured interval; keep the kernel's RT limit enabled. */
    while (nanosleep(&idle, &remaining) != 0) {
        if (errno != EINTR) {
            perror("nanosleep(RT budget recovery)");
            exit(EXIT_FAILURE);
        }
        idle = remaining;
    }

    check_pthread(pthread_mutexattr_init(&attributes), "mutexattr_init");
    check_pthread(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_NORMAL),
                  "mutexattr_settype");
    check_pthread(pthread_mutexattr_setprotocol(&attributes, protocol),
                  "mutexattr_setprotocol");
    check_pthread(pthread_mutexattr_getprotocol(&attributes, &observed_protocol),
                  "mutexattr_getprotocol");
    if (observed_protocol != protocol) {
        fprintf(stderr, "Mutex protocol mismatch\n");
        exit(EXIT_FAILURE);
    }
    check_pthread(pthread_mutex_init(&trial.lock, &attributes), "mutex_init");
    check_pthread(pthread_mutexattr_destroy(&attributes), "mutexattr_destroy");

    for (int i = 0; i < WORKERS; ++i) {
        check_system(sem_init(&trial.ready[i], 0, 0), "sem_init(ready)");
        check_system(sem_init(&trial.gate[i], 0, 0), "sem_init(gate)");
        trial.workers[i].trial = &trial;
        trial.workers[i].role = (enum role)i;
        check_pthread(pthread_create(&threads[i], NULL, run_worker,
                                     &trial.workers[i]), "pthread_create");
    }
    for (int i = 0; i < WORKERS; ++i)
        wait_gate(&trial.ready[i]);
    check_system(sem_post(&trial.gate[HIGH]), "sem_post(H)");

    const enum role join_order[] = { HIGH, LOW, MEDIUM };
    for (int i = 0; i < WORKERS; ++i) {
        struct timespec until = deadline();
        check_pthread(pthread_timedjoin_np(threads[join_order[i]], NULL, &until),
                      "pthread_timedjoin_np");
    }
    check_pthread(pthread_mutex_destroy(&trial.lock), "mutex_destroy");
    for (int i = 0; i < WORKERS; ++i) {
        check_system(sem_destroy(&trial.ready[i]), "sem_destroy(ready)");
        check_system(sem_destroy(&trial.gate[i]), "sem_destroy(gate)");
    }
    check_order(&trial, pi);

    printf("%s,%d,%d,%d,%d,%d,%d,%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",PASS\n",
           pi ? "pi" : "none", round, worker_cpu, controller_cpu,
           trial.workers[LOW].tid, trial.workers[MEDIUM].tid,
           trial.workers[HIGH].tid,
           trial.high_acquired - trial.high_requested,
           trial.low_cpu_time, trial.medium_cpu_time);
    fflush(stdout);
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [both|pi|none] [rounds: 1..100]\n"
            "Requires two available CPUs and permission for SCHED_FIFO.\n"
            "Fixed priorities L/M/H=10/20/30; CPU work L/M=20/100 ms.\n"
            "Requires RT period/runtime=1000000/950000 us; idles 1 s per trial.\n"
            "No-PI: M completes before L computes. PI: L unlocks before M "
            "completes.\n", program);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "both";
    int rounds = 5;
    cpu_set_t available, control_mask;
    int worker_cpu = -1, controller_cpu = -1;

    if (argc == 2 && strcmp(mode, "--help") == 0) {
        usage(argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc > 3 || (strcmp(mode, "both") != 0 &&
                    strcmp(mode, "pi") != 0 && strcmp(mode, "none") != 0)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 3) {
        char *end;
        errno = 0;
        long value = strtol(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' ||
            value < 1 || value > 100) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        rounds = (int)value;
    }
    long period_us = read_rt_setting("/proc/sys/kernel/sched_rt_period_us");
    long runtime_us = read_rt_setting("/proc/sys/kernel/sched_rt_runtime_us");
    if (period_us != 1000000 || runtime_us != 950000) {
        fprintf(stderr, "Baseline requires RT period/runtime=1000000/950000 us; "
                "observed %ld/%ld us. No settings were changed.\n",
                period_us, runtime_us);
        return EXIT_FAILURE;
    }
    printf("# RT period_us=%ld runtime_us=%ld recovery_gap_ms=1000\n",
           period_us, runtime_us);
    check_system(sched_getaffinity(0, sizeof(available), &available),
                 "sched_getaffinity");
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &available))
            continue;
        if (worker_cpu == -1)
            worker_cpu = cpu;
        else {
            controller_cpu = cpu;
            break;
        }
    }
    if (controller_cpu == -1) {
        fprintf(stderr, "Need at least two available CPUs\n");
        return EXIT_FAILURE;
    }
    CPU_ZERO(&control_mask);
    CPU_SET(controller_cpu, &control_mask);
    check_system(sched_setaffinity(0, sizeof(control_mask), &control_mask),
                 "sched_setaffinity(controller)");

    puts("# Single-lock PI baseline; requested FIFO L/M/H=10/20/30; "
         "PASS rows have verified policy and affinity");
    puts("protocol,round,worker_cpu,controller_cpu,low_tid,medium_tid,high_tid,"
         "high_wait_ns,low_cpu_ns,medium_cpu_ns,order");
    fflush(stdout);
    for (int round = 1; round <= rounds; ++round) {
        if (strcmp(mode, "both") == 0) {
            /* Alternate pair order to reduce a fixed warm-up/order bias. */
            bool first_pi = round % 2 == 0;
            run_trial(first_pi, round, worker_cpu, controller_cpu);
            run_trial(!first_pi, round, worker_cpu, controller_cpu);
        } else {
            run_trial(strcmp(mode, "pi") == 0, round, worker_cpu, controller_cpu);
        }
    }
    return EXIT_SUCCESS;
}
