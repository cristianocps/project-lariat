#ifndef LIBC_TIME_H
#define LIBC_TIME_H

#include <stdint.h>
#include "libc/unistd.h"

typedef long time_t;

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

static inline int clock_gettime(int clk, struct timespec *ts) {
    return (int)__syscall_ret(syscall2(SYS_CLOCK_GETTIME, clk, (long)ts));
}

static inline int gettimeofday(struct timeval *tv, void *tz) {
    return (int)__syscall_ret(syscall2(SYS_GETTIMEOFDAY, (long)tv, (long)tz));
}

static inline int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)__syscall_ret(syscall2(SYS_NANOSLEEP, (long)req, (long)rem));
}

static inline unsigned int sleep(unsigned int seconds) {
    struct timespec ts = { (int64_t)seconds, 0 };
    nanosleep(&ts, (struct timespec *)0);
    return 0;
}

static inline int usleep(unsigned long usec) {
    struct timespec ts = { (int64_t)(usec / 1000000ul),
                           (int64_t)((usec % 1000000ul) * 1000ul) };
    return nanosleep(&ts, (struct timespec *)0);
}

static inline time_t time(time_t *tp) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return (time_t)-1;
    if (tp) *tp = (time_t)ts.tv_sec;
    return (time_t)ts.tv_sec;
}

#endif /* LIBC_TIME_H */
