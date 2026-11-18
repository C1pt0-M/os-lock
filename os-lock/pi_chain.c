// SPDX-License-Identifier: GPL-2.0-only
/*
 * Two-lock inheritance check for the fixed Linux 5.10.274 baseline:
 *     timeout 30 /bin/pi_chain none
 *     timeout 30 /bin/pi_chain pi
 * X owns B; L owns A and blocks on B; H then requests A. M only computes.
 * /proc's kernel stack confirms L's waiting path before H starts. Workers share one
 * CPU; the controller uses another. This validates existing PI, not an
 * optimization. It does not cover cancellation or arbitrary chain depth.
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

enum role { END_OWNER, LOW, MEDIUM, HIGH, WORKERS };
static const int priorities[WORKERS] = { 10, 20, 30, 40 };

struct trial {
    pthread_mutex_t a, b;
    sem_t ready[WORKERS];
    sem_t end_gate, medium_gate, high_gate;
    int worker_cpu;
    bool pi;
    pid_t tids[WORKERS];
    uint64_t low_locked, low_requested_b, low_acquired_b;
    uint64_t end_started, end_unlock_begin, end_cpu;
    uint64_t medium_started, medium_finished, medium_cpu;
    uint64_t high_requested, high_acquired;
};
struct worker { struct trial *trial; enum role role; };

static void check(int error, const char *operation)
{
    if (error) {
        fprintf(stderr, "%s: %s (error %d)\n", operation, strerror(error), error);
        exit(EXIT_FAILURE);
    }
}

static void check_system(int result, const char *operation)
{
    if (result == -1)
        check(errno, operation);
}

static long read_setting(const char *path)
{
    FILE *file = fopen(path, "r");
    long value;
    if (!file) {
        perror(path);
        exit(EXIT_FAILURE);
    }
    if (fscanf(file, "%ld", &value) != 1) {
        fprintf(stderr, "Cannot read setting: %s\n", path);
        exit(EXIT_FAILURE);
    }
    fclose(file);
    return value;
}

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec ts;
    check_system(clock_gettime(clock, &ts), "clock_gettime");
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static void pause_ns(long ns)
{
    struct timespec request = { ns / 1000000000L, ns % 1000000000L };
    struct timespec remaining;
    int error;
    while ((error = clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remaining))) {
        if (error != EINTR)
            check(error, "clock_nanosleep");
        request = remaining;
    }
}

static struct timespec deadline(void)
{
    struct timespec ts;
    check_system(clock_gettime(CLOCK_REALTIME, &ts), "clock_gettime(deadline)");
    ts.tv_sec += 10; /* Allow the controller's 5-second stack observation. */
    return ts;
}

static void wait_gate(sem_t *gate)
{
    struct timespec until = deadline();
    while (sem_timedwait(gate, &until)) {
        if (errno != EINTR)
            check(errno, "sem_timedwait");
    }
}

static uint64_t burn_cpu(uint64_t duration)
{
    volatile uint64_t value = 1;
    uint64_t start = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    uint64_t elapsed;
    do {
        for (int i = 0; i < 256; ++i)
            value = value * UINT64_C(6364136223846793005) + 1;
        elapsed = clock_ns(CLOCK_THREAD_CPUTIME_ID) - start;
    } while (elapsed < duration);
    return elapsed;
}

static void *run_worker(void *argument)
{
    struct worker *worker = argument;
    struct trial *s = worker->trial;
    enum role role = worker->role;
    cpu_set_t mask;
    struct sched_param priority = { .sched_priority = priorities[role] };
    struct sched_param actual;
    int policy;

    CPU_ZERO(&mask);
    CPU_SET(s->worker_cpu, &mask);
    check(pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask),
          "pthread_setaffinity_np");
    check(pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority),
          "pthread_setschedparam(SCHED_FIFO)");
    check(pthread_getschedparam(pthread_self(), &policy, &actual),
          "pthread_getschedparam");
    if (policy != SCHED_FIFO || actual.sched_priority != priorities[role]) {
        fprintf(stderr, "Worker scheduling mismatch\n");
        exit(EXIT_FAILURE);
    }
    s->tids[role] = (pid_t)syscall(SYS_gettid);

    if (role == END_OWNER)
        check(pthread_mutex_lock(&s->b), "X: lock B");
    if (role == LOW) {
        check(pthread_mutex_lock(&s->a), "L: lock A");
        s->low_locked = clock_ns(CLOCK_MONOTONIC);
    }
    check_system(sem_post(&s->ready[role]), "sem_post(ready)");

    if (role == LOW) {
        s->low_requested_b = clock_ns(CLOCK_MONOTONIC);
        check(pthread_mutex_lock(&s->b), "L: lock B");
        s->low_acquired_b = clock_ns(CLOCK_MONOTONIC);
        check(pthread_mutex_unlock(&s->b), "L: unlock B");
        check(pthread_mutex_unlock(&s->a), "L: unlock A");
        return NULL;
    }
    if (role == END_OWNER) {
        wait_gate(&s->end_gate);
        s->end_started = clock_ns(CLOCK_MONOTONIC);
        s->end_cpu = burn_cpu(UINT64_C(20000000));
        s->end_unlock_begin = clock_ns(CLOCK_MONOTONIC);
        check(pthread_mutex_unlock(&s->b), "X: unlock B");
        return NULL;
    }
    if (role == MEDIUM) {
        wait_gate(&s->medium_gate);
        s->medium_started = clock_ns(CLOCK_MONOTONIC);
        s->medium_cpu = burn_cpu(UINT64_C(100000000));
        s->medium_finished = clock_ns(CLOCK_MONOTONIC);
        return NULL;
    }
    wait_gate(&s->high_gate);
    /* H is the highest-priority worker: release both competitors here. */
    check_system(sem_post(&s->medium_gate), "H: release M");
    check_system(sem_post(&s->end_gate), "H: release X");
    s->high_requested = clock_ns(CLOCK_MONOTONIC);
    check(pthread_mutex_lock(&s->a), "H: lock A");
    s->high_acquired = clock_ns(CLOCK_MONOTONIC);
    check(pthread_mutex_unlock(&s->a), "H: unlock A");
    return NULL;
}

static void observe_low_blocked(const struct trial *s)
{
    char path[128], frame[256];
    const char *expected = s->pi ? "rt_mutex_wait_proxy_lock+" : "futex_wait_queue_me+";
    uint64_t until = clock_ns(CLOCK_MONOTONIC) + UINT64_C(5000000000);
    /* This kernel's wchan walk needs frame pointers; /stack supports ORC. */
    snprintf(path, sizeof(path), "/proc/self/task/%d/stack", s->tids[LOW]);

    do {
        FILE *file = fopen(path, "r");
        bool found = false;
        if (!file) {
            perror(path);
            exit(EXIT_FAILURE);
        }
        while (fgets(frame, sizeof(frame), file)) {
            if (strstr(frame, expected)) {
                found = true;
                break;
            }
        }
        if (ferror(file)) {
            fprintf(stderr, "Cannot read L's kernel stack\n");
            exit(EXIT_FAILURE);
        }
        fclose(file);
        if (found) {
            printf("# L tid=%d observed_blocked=%s in kernel stack before H starts\n",
                   s->tids[LOW], expected);
            return;
        }
        pause_ns(1000000L);
    } while (clock_ns(CLOCK_MONOTONIC) < until);
    fprintf(stderr, "L was not observed at kernel frame %s\n", expected);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("Usage: pi_chain pi|none (one trial; requires two CPUs and RT permission)");
        return EXIT_SUCCESS;
    }
    if (argc != 2 || (strcmp(argv[1], "pi") && strcmp(argv[1], "none"))) {
        fprintf(stderr, "Usage: %s pi|none\n", argv[0]);
        return EXIT_FAILURE;
    }
    long period = read_setting("/proc/sys/kernel/sched_rt_period_us");
    long runtime = read_setting("/proc/sys/kernel/sched_rt_runtime_us");
    if (period != 1000000 || runtime != 950000) {
        fprintf(stderr, "Requires RT period/runtime=1000000/950000 us; got %ld/%ld\n",
                period, runtime);
        return EXIT_FAILURE;
    }
    cpu_set_t available, control;
    int cpu = -1, controller = -1;
    check_system(sched_getaffinity(0, sizeof(available), &available), "sched_getaffinity");
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (!CPU_ISSET(i, &available))
            continue;
        if (cpu == -1)
            cpu = i;
        else {
            controller = i;
            break;
        }
    }
    if (controller == -1) {
        fprintf(stderr, "Need at least two available CPUs\n");
        return EXIT_FAILURE;
    }
    CPU_ZERO(&control);
    CPU_SET(controller, &control);
    check_system(sched_setaffinity(0, sizeof(control), &control), "controller affinity");
    pause_ns(1000000000L);

    struct trial s = { .worker_cpu = cpu, .pi = strcmp(argv[1], "pi") == 0 };
    pthread_mutexattr_t attr;
    pthread_t threads[WORKERS];
    struct worker workers[WORKERS];
    int protocol = s.pi ? PTHREAD_PRIO_INHERIT : PTHREAD_PRIO_NONE;
    int observed_protocol;
    check(pthread_mutexattr_init(&attr), "mutexattr_init");
    check(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL), "mutexattr_settype");
    check(pthread_mutexattr_setprotocol(&attr, protocol), "mutexattr_setprotocol");
    check(pthread_mutexattr_getprotocol(&attr, &observed_protocol), "mutexattr_getprotocol");
    if (observed_protocol != protocol) {
        fprintf(stderr, "Mutex protocol mismatch\n");
        return EXIT_FAILURE;
    }
    check(pthread_mutex_init(&s.a, &attr), "mutex_init(A)");
    check(pthread_mutex_init(&s.b, &attr), "mutex_init(B)");
    check(pthread_mutexattr_destroy(&attr), "mutexattr_destroy");
    check_system(sem_init(&s.end_gate, 0, 0), "sem_init(X)");
    check_system(sem_init(&s.medium_gate, 0, 0), "sem_init(M)");
    check_system(sem_init(&s.high_gate, 0, 0), "sem_init(H)");
    for (int i = 0; i < WORKERS; ++i) {
        check_system(sem_init(&s.ready[i], 0, 0), "sem_init(ready)");
        workers[i].trial = &s;
        workers[i].role = (enum role)i;
        check(pthread_create(&threads[i], NULL, run_worker, &workers[i]), "pthread_create");
        /* X owns B before L starts; all workers wait before H is released. */
        wait_gate(&s.ready[i]);
    }
    observe_low_blocked(&s);
    check_system(sem_post(&s.high_gate), "controller: release H");
    const enum role join_order[] = { HIGH, LOW, END_OWNER, MEDIUM };
    for (int i = 0; i < WORKERS; ++i) {
        struct timespec until = deadline();
        check(pthread_timedjoin_np(threads[join_order[i]], NULL, &until), "timedjoin");
    }
    check(pthread_mutex_destroy(&s.a), "mutex_destroy(A)");
    check(pthread_mutex_destroy(&s.b), "mutex_destroy(B)");
    check_system(sem_destroy(&s.end_gate), "sem_destroy(X)");
    check_system(sem_destroy(&s.medium_gate), "sem_destroy(M)");
    check_system(sem_destroy(&s.high_gate), "sem_destroy(H)");
    for (int i = 0; i < WORKERS; ++i)
        check_system(sem_destroy(&s.ready[i]), "sem_destroy(ready)");

    bool timestamps = s.low_locked <= s.low_requested_b &&
        s.low_requested_b <= s.high_requested && s.high_requested <= s.end_started &&
        s.high_requested <= s.medium_started && s.end_started <= s.end_unlock_begin &&
        s.end_unlock_begin <= s.low_acquired_b && s.low_acquired_b <= s.high_acquired &&
        s.medium_started <= s.medium_finished;
    bool order = s.pi ? s.end_unlock_begin < s.medium_finished
                     : s.medium_finished <= s.end_started;
    if (!timestamps || !order) {
        fprintf(stderr, "INVALID: two-lock execution order did not match the protocol\n");
        return EXIT_FAILURE;
    }
    puts("protocol,worker_cpu,controller_cpu,end_tid,low_tid,medium_tid,high_tid,"
         "high_wait_ns,low_b_wait_ns,end_cpu_ns,medium_cpu_ns,order");
    printf("%s,%d,%d,%d,%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",PASS\n", argv[1], cpu, controller,
           s.tids[END_OWNER], s.tids[LOW], s.tids[MEDIUM], s.tids[HIGH],
           s.high_acquired - s.high_requested, s.low_acquired_b - s.low_requested_b,
           s.end_cpu, s.medium_cpu);
    return EXIT_SUCCESS;
}
