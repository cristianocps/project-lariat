#include "sched.h"
#include "kapi.h"
#include "serial.h"
#include "idt.h"
#include "pic.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Scheduler state
 * -------------------------------------------------------------------------- */
static struct thread *current = NULL;
static struct thread *ready_queue_head = NULL;
static struct thread *ready_queue_tail = NULL;
static struct thread *sleep_list = NULL;
static uint32_t next_tid = 1;
static spinlock_t sched_lock = SPINLOCK_INIT;

/* Idle thread runs when nothing else is ready */
static struct thread idle_thread;

/* --------------------------------------------------------------------------
 * Queue helpers
 * -------------------------------------------------------------------------- */
static void queue_push(struct thread **head, struct thread **tail, struct thread *t) {
    t->next = NULL;
    if (*tail) {
        (*tail)->next = t;
    } else {
        *head = t;
    }
    *tail = t;
}

static struct thread *queue_pop(struct thread **head, struct thread **tail) {
    struct thread *t = *head;
    if (t) {
        *head = t->next;
        if (!*head) *tail = NULL;
        t->next = NULL;
    }
    return t;
}

static void queue_remove(struct thread **head, struct thread **tail, struct thread *t) {
    struct thread **pp = head;
    while (*pp) {
        if (*pp == t) {
            *pp = t->next;
            if (!*pp) *tail = NULL;
            t->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* --------------------------------------------------------------------------
 * Internal: allocate and free thread stacks
 * -------------------------------------------------------------------------- */
#define THREAD_STACK_SIZE  8192   /* 8 KB */

static uint8_t *alloc_stack(void) {
    /* Use alloc_pages for page-aligned, physically contiguous memory */
    void *pages = alloc_pages(2);  /* 2 pages = 8KB */
    if (!pages) return NULL;
    return (uint8_t *)pages + THREAD_STACK_SIZE;  /* stack grows down */
}

static void free_stack(uint8_t *top) {
    free_pages(top - THREAD_STACK_SIZE, 2);
}

/* --------------------------------------------------------------------------
 * Thread trampoline
 * -------------------------------------------------------------------------- */
static void thread_trampoline(void) {
    /* Re-enable interrupts: new threads start with IF=0 because they
     * were entered via switch_thread/ret rather than iretq. */
    __asm__ __volatile__("sti");
    struct thread *t = current;
    if (t && t->entry) {
        t->entry(t->arg);
    }
    thread_exit();
}

/* --------------------------------------------------------------------------
 * Thread creation
 * -------------------------------------------------------------------------- */
struct thread *thread_create(void (*entry)(void *), void *arg) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);

    struct thread *t = kzalloc(sizeof(struct thread));
    if (!t) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return NULL;
    }

    t->tid = next_tid++;
    t->state = THREAD_READY;
    t->entry = entry;
    t->arg = arg;
    t->stack_size = THREAD_STACK_SIZE;

    uint8_t *stack_top = alloc_stack();
    if (!stack_top) {
        kfree(t);
        spin_unlock_irqrestore(&sched_lock, flags);
        return NULL;
    }
    t->stack_base = (uint64_t)stack_top;

    /*
     * Set up initial stack for switch_thread:
     *
     * switch_thread pops in this order: r15, r14, r13, r12, rbp, rbx
     * then ret to the return address on stack.
     */
    uint64_t *sp = (uint64_t *)stack_top;
    *--sp = (uint64_t)thread_trampoline;  /* return address */
    *--sp = 0;  /* rbx */
    *--sp = 0;  /* rbp */
    *--sp = 0;  /* r12 */
    *--sp = 0;  /* r13 */
    *--sp = 0;  /* r14 */
    *--sp = 0;  /* r15 */

    t->rsp = (uint64_t)sp;

    queue_push(&ready_queue_head, &ready_queue_tail, t);

    spin_unlock_irqrestore(&sched_lock, flags);
    return t;
}

/* --------------------------------------------------------------------------
 * Thread exit
 * -------------------------------------------------------------------------- */
void thread_exit(void) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);

    if (current) {
        current->state = THREAD_ZOMBIE;
        /* Remove from ready queue if present */
        queue_remove(&ready_queue_head, &ready_queue_tail, current);
    }

    /* Pick next thread */
    struct thread *next = queue_pop(&ready_queue_head, &ready_queue_tail);
    if (!next) {
        next = &idle_thread;
    }

    struct thread *prev = current;
    current = next;
    current->state = THREAD_RUNNING;

    spin_unlock_irqrestore(&sched_lock, flags);

    /* Switch to next thread. We never return from this call. */
    switch_thread(&prev->rsp, current->rsp);

    /* Should never reach here */
    __asm__ __volatile__("cli; hlt");
}

/* --------------------------------------------------------------------------
 * Thread yield
 * -------------------------------------------------------------------------- */
void thread_yield(void) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);

    if (current && current->state == THREAD_RUNNING) {
        current->state = THREAD_READY;
        queue_push(&ready_queue_head, &ready_queue_tail, current);
    }

    struct thread *next = queue_pop(&ready_queue_head, &ready_queue_tail);
    if (!next) {
        next = &idle_thread;
    }

    if (next == current) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    struct thread *prev = current;
    current = next;
    current->state = THREAD_RUNNING;

    spin_unlock_irqrestore(&sched_lock, flags);

    switch_thread(&prev->rsp, current->rsp);
}

/* --------------------------------------------------------------------------
 * Thread sleep
 * -------------------------------------------------------------------------- */
void thread_sleep(uint64_t ticks) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);

    extern uint64_t timer_get_ticks(void);
    uint64_t wakeup = timer_get_ticks() + ticks;

    if (current) {
        current->state = THREAD_BLOCKED;
        current->wakeup_tick = wakeup;
        current->next = sleep_list;
        sleep_list = current;
    }

    struct thread *next = queue_pop(&ready_queue_head, &ready_queue_tail);
    if (!next) {
        next = &idle_thread;
    }

    struct thread *prev = current;
    current = next;
    current->state = THREAD_RUNNING;

    spin_unlock_irqrestore(&sched_lock, flags);

    switch_thread(&prev->rsp, current->rsp);
}

/* --------------------------------------------------------------------------
 * Scheduler tick: called from timer IRQ
 * -------------------------------------------------------------------------- */
void scheduler_tick(registers_t *r) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);

    /* If scheduler not initialized yet, just return */
    if (!current) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    /* On first tick, capture the idle thread's context */
    if (current->rsp == 0) {
        current->rsp = (uint64_t)r;
    }

    /* Wake up sleeping threads */
    extern uint64_t timer_get_ticks(void);
    uint64_t now = timer_get_ticks();
    struct thread **pp = &sleep_list;
    while (*pp) {
        struct thread *t = *pp;
        if (t->wakeup_tick <= now) {
            *pp = t->next;
            t->state = THREAD_READY;
            t->next = NULL;
            queue_push(&ready_queue_head, &ready_queue_tail, t);
        } else {
            pp = &t->next;
        }
    }

    /* If current is running, move to end of queue for round-robin */
    if (current->state == THREAD_RUNNING && current != &idle_thread) {
        current->state = THREAD_READY;
        queue_push(&ready_queue_head, &ready_queue_tail, current);
    }

    struct thread *next = queue_pop(&ready_queue_head, &ready_queue_tail);
    if (!next) {
        next = &idle_thread;
    }

    if (next == current) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    struct thread *prev = current;
    current = next;
    current->state = THREAD_RUNNING;

    spin_unlock_irqrestore(&sched_lock, flags);

    switch_thread(&prev->rsp, current->rsp);
}

/* --------------------------------------------------------------------------
 * Scheduler init
 * -------------------------------------------------------------------------- */
void scheduler_init(void) {
    /* Mark kmain as the idle thread */
    memset(&idle_thread, 0, sizeof(idle_thread));
    idle_thread.tid = 0;
    idle_thread.state = THREAD_RUNNING;
    current = &idle_thread;

    serial_printf(SERIAL_COM1, "[SCHED] Scheduler initialized, idle thread is kmain\n");
}

/* --------------------------------------------------------------------------
 * Accessor
 * -------------------------------------------------------------------------- */
struct thread *current_thread(void) {
    return current;
}
