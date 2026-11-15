#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#define NUM_THREADS 3
#define ITERATIONS 10000

pthread_mutex_t lock;
// 用于记录时延数据的数组（注意：多线程同时写可能有轻微偏差，但不影响大体趋势）
long long latencies[NUM_THREADS][ITERATIONS];

// 获取当前纳秒级时间
long long get_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void *thread_func(void *arg) {
    int id = *(int *)arg;
    
    // 设置线程为实时调度策略（FIFO），并赋予不同优先级（需要root权限，在QEMU里默认就是）
    struct sched_param param;
    param.sched_priority = 10 + id; // 优先级 10, 11, 12
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    for (int i = 0; i < ITERATIONS; i++) {
        long long start = get_ns();
        pthread_mutex_lock(&lock);
        long long end = get_ns();
        
        latencies[id][i] = end - start;
        
        // 模拟极短的临界区工作
        // usleep(1); 
        
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

// 简单的比较函数用于排序算分位数
int compare(const void *a, const void *b) {
    long long diff = *(long long*)a - *(long long*)b;
    return (diff > 0) - (diff < 0);
}

int main() {
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    // 初始化锁属性，开启优先级继承 (PI)
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT); 
    pthread_mutex_init(&lock, &attr);

    printf("Starting Benchmark with %d threads, %d iterations...\n", NUM_THREADS, ITERATIONS);

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, thread_func, &thread_ids[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // 统计并打印结果
    printf("\n--- Latency Results (nanoseconds) ---\n");
    for (int i = 0; i < NUM_THREADS; i++) {
        // 排序
        qsort(latencies[i], ITERATIONS, sizeof(long long), compare);
        
        long long p50 = latencies[i][ITERATIONS * 50 / 100];
        long long p90 = latencies[i][ITERATIONS * 90 / 100];
        long long p99 = latencies[i][ITERATIONS * 99 / 100];
        long long max = latencies[i][ITERATIONS - 1];

        printf("Thread %d (Priority %d):\n", i, 10 + i);
        printf("  P50: %lld ns\n", p50);
        printf("  P90: %lld ns\n", p90);
        printf("  P99: %lld ns\n", p99);
        printf("  Max: %lld ns\n", max);
    }

    pthread_mutex_destroy(&lock);
    return 0;
}