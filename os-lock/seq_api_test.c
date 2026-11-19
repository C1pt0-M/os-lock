// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) errx(1, "%s:%d: %s", __FILE__, __LINE__, #expr); } while (0)
#define ROUNDS 500

static int open_fd(const char *path, int flags)
{
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0)
        err(1, "open %s", path);
    return fd;
}

struct worker {
    int fd, seeker, id;
    const char *expected;
    size_t len;
    pthread_barrier_t *start;
};

static void *exercise(void *arg)
{
    struct worker *w = arg;
    int rc = pthread_barrier_wait(w->start);
    CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
    for (size_t i = 0; i < ROUNDS; ++i) {
        char buf[31];
        off_t off = (i * 53 + w->id * 19) % (w->len - sizeof(buf));
        if (w->seeker) {
            CHECK(lseek(w->fd, off, SEEK_SET) == off);
            CHECK(read(w->fd, buf, sizeof(buf)) == sizeof(buf));
        } else {
            CHECK(pread(w->fd, buf, sizeof(buf), off) == sizeof(buf));
        }
        CHECK(!memcmp(buf, w->expected + off, sizeof(buf)));
    }
    return NULL;
}

static void check_reader(const char *path, const char *token, int concurrent)
{
    char expected[32768], buf[61];
    int fd = open_fd(path, O_RDONLY);
    ssize_t n, len = 0;
    while ((n = read(fd, expected + len, sizeof(expected) - 1 - len)) > 0)
        len += n;
    CHECK(n == 0 && len > (ssize_t)sizeof(buf) && len < (ssize_t)sizeof(expected) - 1);
    expected[len] = '\0';
    CHECK(strstr(expected, token));
    CHECK(lseek(fd, 0, SEEK_CUR) == len);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    for (ssize_t offset = 0; offset < len; offset += n) {
        n = read(fd, buf, 7);
        CHECK(n > 0 && offset + n <= len && !memcmp(buf, expected + offset, n));
    }
    CHECK(read(fd, buf, sizeof(buf)) == 0);
    CHECK(lseek(fd, 13, SEEK_SET) == 13);
    CHECK(lseek(fd, 7, SEEK_CUR) == 20);
    CHECK(pread(fd, buf, sizeof(buf), 5) == sizeof(buf));
    CHECK(!memcmp(buf, expected + 5, sizeof(buf)));
    CHECK(lseek(fd, 0, SEEK_CUR) == 20);
    ssize_t want = len - 20 < (ssize_t)sizeof(buf) ? len - 20 : (ssize_t)sizeof(buf);
    CHECK(read(fd, buf, sizeof(buf)) == want);
    CHECK(!memcmp(buf, expected + 20, want));
    CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL);
    CHECK(lseek(fd, 0, SEEK_END) == -1 && errno == EINVAL);
    CHECK(write(fd, "x", 1) == -1 && errno == EBADF);
    printf("SEQ_READ,%s,PASS\n", path);

    if (concurrent) {
        pthread_t threads[4];
        pthread_barrier_t start;
        struct worker jobs[4];
        int separate = open_fd(path, O_RDONLY);
        CHECK(pthread_barrier_init(&start, NULL, 4) == 0);
        for (int i = 0; i < 4; ++i) {
            jobs[i] = (struct worker){ i == 3 ? separate : fd, i == 0, i, expected, len, &start };
            CHECK(pthread_create(&threads[i], NULL, exercise, &jobs[i]) == 0);
        }
        for (int i = 0; i < 4; ++i)
            CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(pthread_barrier_destroy(&start) == 0);
        CHECK(close(separate) == 0);
        printf("SEQ_CONCURRENT,%s,%d,PASS\n", path, 4 * ROUNDS);
    }
    CHECK(close(fd) == 0);
}

int main(void)
{
    check_reader("/proc/version", "Linux version", 0);
    check_reader("/proc/filesystems", "proc\n", 1);
    check_reader("/proc/self/mountinfo", " - proc ", 1);

    int fd = open_fd("/proc/self/comm", O_RDWR);
    char original[32], buf[32];
    const char name[] = "seq-pi-check";
    ssize_t n = read(fd, original, sizeof(original));
    CHECK(n > 1 && original[n - 1] == '\n');
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(write(fd, name, sizeof(name) - 1) == sizeof(name) - 1);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, buf, sizeof(buf)) == sizeof(name));
    CHECK(!memcmp(buf, name, sizeof(name) - 1) && buf[sizeof(name) - 1] == '\n');
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(write(fd, original, n - 1) == n - 1);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, buf, sizeof(buf)) == n && !memcmp(buf, original, n));
    CHECK(close(fd) == 0);
    puts("SEQ_WRITE,/proc/self/comm,PASS");
    return 0;
}
