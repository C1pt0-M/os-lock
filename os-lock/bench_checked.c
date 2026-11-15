// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NUM_THREADS 3
#ifndef ITERATIONS
#define ITERATIONS 500000
#endif
#ifndef SLEEP_NS
#define SLEEP_NS 1000
#endif

_Static_assert(ITERATIONS > 0 && ITERATIONS <= 1000000,
               "ITERATIONS must be between 1 and 1000000");
_Static_assert(SLEEP_NS >= 0 && SLEEP_NS < 1000000000,
               "SLEEP_NS must be between 0 and 999999999");

static pthread_mutex_t lock;
static uint64_t latencies[NUM_THREADS][ITERATIONS];

struct worker {
    int id;
    int actual_policy;
    int actual_priority;
};

static void check_pthread(int error, const char *operation)
{
    if (error != 0) {
        fprintf(stderr, "%s: %s (error %d)\n", operation,
                strerror(error), error);
        exit(EXIT_FAILURE);
    }
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void sleep_in_critical_section(void)
{
    struct timespec request = { .tv_sec = 0, .tv_nsec = SLEEP_NS };
    struct timespec remaining;

    if (SLEEP_NS == 0)
        return;

    while (nanosleep(&request, &remaining) != 0) {
        if (errno != EINTR) {
            perror("nanosleep");
            exit(EXIT_FAILURE);
        }
        request = remaining;
    }
}

static void *run_worker(void *argument)
{
    struct worker *worker = argument;
    struct sched_param requested = { .sched_priority = 10 + worker->id };
    struct sched_param actual;

    check_pthread(pthread_setschedparam(pthread_self(), SCHED_FIFO, &requested),
                  "pthread_setschedparam(SCHED_FIFO)");
    check_pthread(pthread_getschedparam(pthread_self(), &worker->actual_policy,
                                      &actual),
                  "pthread_getschedparam");
    worker->actual_priority = actual.sched_priority;
    if (worker->actual_policy != SCHED_FIFO ||
        worker->actual_priority != requested.sched_priority) {
        fprintf(stderr, "Thread %d: scheduling policy/priority mismatch\n",
                worker->id);
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < ITERATIONS; ++i) {
        uint64_t start = now_ns();
        check_pthread(pthread_mutex_lock(&lock), "pthread_mutex_lock");
        uint64_t end = now_ns();

        if (end < start) {
            fprintf(stderr, "CLOCK_MONOTONIC moved backwards\n");
            exit(EXIT_FAILURE);
        }
        latencies[worker->id][i] = end - start;
        sleep_in_critical_section();
        check_pthread(pthread_mutex_unlock(&lock), "pthread_mutex_unlock");
    }
    return NULL;
}

static int compare_latency(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return (a > b) - (a < b);
}

int main(void)
{
    pthread_mutexattr_t attributes;
    pthread_t threads[NUM_THREADS];
    struct worker workers[NUM_THREADS];
    int protocol;

    check_pthread(pthread_mutexattr_init(&attributes), "pthread_mutexattr_init");
    check_pthread(pthread_mutexattr_setprotocol(&attributes, PTHREAD_PRIO_INHERIT),
                  "pthread_mutexattr_setprotocol");
    check_pthread(pthread_mutexattr_getprotocol(&attributes, &protocol),
                  "pthread_mutexattr_getprotocol");
    if (protocol != PTHREAD_PRIO_INHERIT) {
        fprintf(stderr, "Mutex protocol is not PTHREAD_PRIO_INHERIT\n");
        return EXIT_FAILURE;
    }
    check_pthread(pthread_mutex_init(&lock, &attributes), "pthread_mutex_init");
    check_pthread(pthread_mutexattr_destroy(&attributes), "pthread_mutexattr_destroy");

    printf("Checked workload: PI mutex, %d threads, %d iterations/thread, "
           "sleep request %d ns\n", NUM_THREADS, ITERATIONS, SLEEP_NS);
    fflush(stdout);

    for (int i = 0; i < NUM_THREADS; ++i) {
        workers[i].id = i;
        check_pthread(pthread_create(&threads[i], NULL, run_worker, &workers[i]),
                      "pthread_create");
    }
    for (int i = 0; i < NUM_THREADS; ++i)
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");

    puts("\n--- Checked latency results (nanoseconds) ---");
    for (int i = 0; i < NUM_THREADS; ++i) {
        qsort(latencies[i], ITERATIONS, sizeof(latencies[i][0]), compare_latency);
        printf("Thread %d (verified policy SCHED_FIFO, priority %d):\n", i,
               workers[i].actual_priority);
        printf("  P50: %" PRIu64 " ns\n", latencies[i][ITERATIONS * 50 / 100]);
        printf("  P90: %" PRIu64 " ns\n", latencies[i][ITERATIONS * 90 / 100]);
        printf("  P99: %" PRIu64 " ns\n", latencies[i][ITERATIONS * 99 / 100]);
        printf("  Max: %" PRIu64 " ns\n", latencies[i][ITERATIONS - 1]);
    }

    check_pthread(pthread_mutex_destroy(&lock), "pthread_mutex_destroy");
    return EXIT_SUCCESS;
}
