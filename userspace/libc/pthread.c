#include "pthread.h"
#include "unistd.h"
#include "errno.h"
#include <stdint.h>

/* clone(2) flag bits (shared with the kernel). */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

#define PTHREAD_STACK_SIZE (64 * 1024)

/* Implemented in clone.S. */
extern long __clone(unsigned long flags, void *child_stack_top,
                    int *ptid, int *ctid, void *tls, void (*entry)(void));

/* The main thread's control block.  Set up lazily the first time any pthread
 * facility is used so that pthread_self() works even before the first
 * pthread_create(). */
static struct __pthread main_tcb;
static int main_tcb_ready;

static inline struct __pthread *tp_get(void) {
    struct __pthread *self;
    __asm__ __volatile__("mov %%fs:0, %0" : "=r"(self));
    return self;
}

static void ensure_main_tcb(void) {
    if (main_tcb_ready) return;
    main_tcb_ready = 1;
    main_tcb.self = &main_tcb;
    main_tcb.tid = gettid();
    main_tcb.ktid = main_tcb.tid;
    arch_prctl(ARCH_SET_FS, (unsigned long)&main_tcb);
}

pthread_t pthread_self(void) {
    ensure_main_tcb();
    return tp_get();
}

/* Child entry: trampolined in from clone.S with the new TLS already installed,
 * so %fs:0 is this thread's TCB. */
void __pthread_start(void);
void __pthread_start(void) {
    struct __pthread *self = tp_get();
    void *ret = self->start(self->arg);
    self->retval = ret;
    /* SYS_EXIT: the kernel zeroes clear_child_tid (&self->tid) and futex-wakes
     * any joiner before tearing the thread down. */
    _exit(0);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg) {
    (void)attr;
    ensure_main_tcb();

    /* One mapping holds both the stack and the TCB (placed at the high end). */
    char *region = (char *)mmap(0, PTHREAD_STACK_SIZE, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)region < 0 && (long)region > -4096) {
        errno = ENOMEM;
        return -1;
    }

    uintptr_t top = (uintptr_t)region + PTHREAD_STACK_SIZE;
    top &= ~(uintptr_t)15;
    struct __pthread *tcb =
        (struct __pthread *)(top - sizeof(struct __pthread));
    tcb = (struct __pthread *)((uintptr_t)tcb & ~(uintptr_t)15);
    void *child_stack = (void *)tcb;     /* stack grows down from just below TCB */

    tcb->self = tcb;
    tcb->tid = 0;
    tcb->ktid = 0;
    tcb->start = start;
    tcb->arg = arg;
    tcb->retval = 0;
    tcb->stack = region;
    tcb->stack_size = PTHREAD_STACK_SIZE;
    tcb->detached = 0;

    unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                          CLONE_THREAD | CLONE_SETTLS |
                          CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;

    long tid = __clone(flags, child_stack, (int *)&tcb->tid, (int *)&tcb->tid,
                       tcb, __pthread_start);
    if (tid < 0) {
        errno = -(int)tid;
        return -1;
    }
    tcb->ktid = (int)tid;
    *thread = tcb;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    if (!thread) return EINVAL;
    /* Wait for the kernel to clear the join word (CLONE_CHILD_CLEARTID). */
    int t;
    while ((t = __atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE)) != 0)
        futex((int *)&thread->tid, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, t, 0);
    /* Reap the zombie so the kernel frees its thread struct and kernel stack. */
    if (thread->ktid > 0) {
        int st;
        waitpid(thread->ktid, &st, 0);
    }
    if (retval) *retval = thread->retval;
    return 0;
}

int pthread_detach(pthread_t thread) {
    if (!thread) return EINVAL;
    thread->detached = 1;
    return 0;
}

void pthread_exit(void *retval) {
    struct __pthread *self = tp_get();
    if (self) self->retval = retval;
    _exit(0);
}

/* ------------------------------------------------------------------------- */
/* Mutex: the classic 3-state futex lock (Drepper).                          */
/* ------------------------------------------------------------------------- */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    m->lock = 0;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    int expected = 0;
    if (__atomic_compare_exchange_n(&m->lock, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    return EBUSY;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    int c = 0;
    if (__atomic_compare_exchange_n(&m->lock, &c, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;                       /* uncontended */
    if (c != 2)
        c = __atomic_exchange_n(&m->lock, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        futex((int *)&m->lock, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 2, 0);
        c = __atomic_exchange_n(&m->lock, 2, __ATOMIC_ACQUIRE);
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (__atomic_fetch_sub(&m->lock, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(&m->lock, 0, __ATOMIC_RELEASE);
        futex((int *)&m->lock, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0);
    }
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }

/* ------------------------------------------------------------------------- */
/* Condition variable: sequence-counter + futex.                             */
/* ------------------------------------------------------------------------- */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    c->seq = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(m);
    futex((int *)&c->seq, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, seq, 0);
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    futex((int *)&c->seq, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    futex((int *)&c->seq, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 0x7fffffff, 0);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
