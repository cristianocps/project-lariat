#ifndef PTHREAD_H
#define PTHREAD_H

#include <stddef.h>

/* Thread control block.  The self-pointer lives at offset 0 so that the x86_64
 * ABI thread pointer (%fs:0) recovers it with a single load. */
struct __pthread {
    struct __pthread *self;          /* %fs:0 */
    volatile int      tid;           /* join futex word: child tid, 0 on exit */
    int               ktid;          /* kernel tid kept for reaping (waitpid) */
    void           *(*start)(void *);
    void             *arg;
    void             *retval;
    void             *stack;         /* mmap base, freed on join */
    size_t            stack_size;
    int               detached;
};

typedef struct __pthread *pthread_t;
typedef int pthread_attr_t;

/* Futex-backed mutex (0=free, 1=held, 2=held+waiters). */
typedef struct { volatile int lock; } pthread_mutex_t;
typedef int pthread_mutexattr_t;
#define PTHREAD_MUTEX_INITIALIZER { 0 }

/* Futex-backed condition variable (monotonic sequence counter). */
typedef struct { volatile int seq; } pthread_cond_t;
typedef int pthread_condattr_t;
#define PTHREAD_COND_INITIALIZER { 0 }

int       pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*start)(void *), void *arg);
int       pthread_join(pthread_t thread, void **retval);
int       pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
void      pthread_exit(void *retval);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_destroy(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_destroy(pthread_cond_t *c);

#endif /* PTHREAD_H */
