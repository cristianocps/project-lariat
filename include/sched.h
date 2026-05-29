#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>
#include "idt.h"

/* --------------------------------------------------------------------------
 * Thread states
 * -------------------------------------------------------------------------- */
enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_ZOMBIE,
};

/* --------------------------------------------------------------------------
 * Thread control block
 * -------------------------------------------------------------------------- */
struct thread {
    uint32_t          tid;
    enum thread_state state;
    uint64_t          rsp;          /* stack pointer when not running */
    uint64_t          stack_base;
    uint64_t          stack_size;
    void            (*entry)(void *arg);
    void             *arg;
    uint64_t          wakeup_tick;  /* for sleep */
    struct thread    *next;
};

/* --------------------------------------------------------------------------
 * Scheduler API
 * -------------------------------------------------------------------------- */
void scheduler_init(void);

struct thread *thread_create(void (*entry)(void *), void *arg);
void thread_yield(void);
void thread_sleep(uint64_t ticks);
void thread_exit(void);

struct thread *current_thread(void);

/* Called from timer IRQ with saved register frame */
void scheduler_tick(registers_t *r);

/* Assembly helper: switch stacks */
extern void switch_thread(uint64_t *old_rsp, uint64_t new_rsp);

#endif /* SCHED_H */
