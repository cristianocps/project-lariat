#include "sched.h"
#include "kapi.h"
#include "serial.h"
#include "idt.h"
#include "pic.h"
#include "gdt.h"
#include "vmm.h"
#include "process.h"
#include "smp.h"
#include "acpi.h"
#include "uapi.h"
#include "msr.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Scheduler state
 *
 * SMP model: a single shared ready queue (protected by sched_lock) feeds every
 * CPU, so any idle core picks up the next runnable thread - this gives natural
 * load balancing across cores.  The *currently running* thread is per-CPU
 * (struct percpu::current) and is found via the LAPIC id (smp_this_percpu()),
 * never via GS, so the same path works on the BSP and the APs.
 *
 * Context-switch locking: the scheduler acquires sched_lock and holds it across
 * switch_thread().  The thread that gets switched IN releases the lock (see
 * sched_finish_switch()).  Holding the lock across the switch is what makes
 * migration safe on SMP: a thread cannot be popped by another CPU until the CPU
 * switching away from it has finished saving its kernel RSP.
 * -------------------------------------------------------------------------- */
static struct thread *ready_queue_head = NULL;
static struct thread *ready_queue_tail = NULL;
static struct thread *sleep_list = NULL;
static uint32_t next_tid = 1;
static spinlock_t sched_lock = SPINLOCK_INIT;

/* One idle thread per CPU; runs when that CPU's share of the ready queue is
 * empty.  Indexed by percpu::cpu_index. */
static struct thread idle_threads[MAX_CPUS];

/* The "reaper" adopts orphaned children when their parent exits (like init/PID
 * 1 on a real system).  Set once the init process exists. */
static struct thread *reaper = NULL;

void sched_set_reaper(struct thread *t) {
    reaper = t;
}

/* Ready-queue helpers (defined below; forward-declared for sched_signal_pgrp). */
static void queue_push(struct thread **head, struct thread **tail, struct thread *t);
static void queue_remove(struct thread **head, struct thread **tail, struct thread *t);
static void sched_ready_locked(struct thread *t);

/* Global registry of all live threads (for kill / find-by-tid). */
static struct thread *all_threads = NULL;

static void sched_register_thread(struct thread *t) {
    t->all_next = all_threads;
    all_threads = t;
}

static void sched_unregister_thread(struct thread *t) {
    struct thread **pp = &all_threads;
    while (*pp) {
        if (*pp == t) { *pp = t->all_next; t->all_next = NULL; return; }
        pp = &(*pp)->all_next;
    }
}

void sched_remove_thread(struct thread *t) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    sched_unregister_thread(t);
    spin_unlock_irqrestore(&sched_lock, flags);
}

struct thread *sched_find_by_tid(uint32_t tid) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct thread *t = all_threads;
    while (t) {
        if (t->tid == tid) break;
        t = t->all_next;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return t;
}

/* Mark `sig` pending on every user thread in process group `pgid`.  Returns the
 * number of threads signalled.  Callers (terminal driver) then wake any blocked
 * readers so the signal is delivered promptly on syscall return. */
int sched_signal_pgrp(int pgid, int sig) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    int n = 0;
    for (struct thread *t = all_threads; t; t = t->all_next) {
        if (t->cr3 && t->pgid == pgid) {
            if (sig == SIGCONT) {
                t->stopped = 0;
                t->stop_reported = 0;
            } else if (t->stopped) {
                /* Let a stopped job run so it can act on the new signal. */
                t->stopped = 0;
            }
            t->sig_pending |= (1ULL << sig);
            /* If it is blocked (syscall wait or job-control stop), wake it so
             * the signal gets delivered / acted on promptly. */
            sched_ready_locked(t);
            n++;
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return n;
}

/* Stopped (job-control) threads park here until SIGCONT re-readies them. */
static wait_queue_t stop_wq = WAIT_QUEUE_INIT;

/* Re-ready a blocked thread regardless of which queue it parked on (wait queue,
 * the timer sleep list, or a bare waitpid block).  Must unlink it from whatever
 * list it sits on first, or it would end up enqueued twice and corrupt the
 * ready queue. */
static void sched_ready_locked(struct thread *t) {
    if (t->state != THREAD_BLOCKED) return;
    if (t->wait_q) {
        queue_remove(&t->wait_q->head, &t->wait_q->tail, t);
        t->wait_q = NULL;
    } else {
        /* It may be parked on the timer sleep list (nanosleep). */
        struct thread **pp = &sleep_list;
        while (*pp) {
            if (*pp == t) { *pp = t->next; break; }
            pp = &(*pp)->next;
        }
    }
    t->state = THREAD_READY;
    t->next = NULL;
    queue_push(&ready_queue_head, &ready_queue_tail, t);
}

void sched_deliver_signal(struct thread *t, int sig) {
    if (!t) return;
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (sig == SIGCONT) {
        /* Resume a stopped job; the SIGCONT itself is also made pending so a
         * user handler (if any) runs on the next syscall return. */
        t->stopped = 0;
        t->stop_reported = 0;
        t->sig_pending |= (1ULL << sig);
        sched_ready_locked(t);
    } else {
        t->sig_pending |= (1ULL << sig);
        /* A stopped target must be re-readied so it can act on the signal
         * (e.g. SIGKILL/SIGTERM); otherwise just wake a blocked syscall. */
        if (t->stopped) t->stopped = 0;
        sched_ready_locked(t);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

/* Block the current thread as job-control stopped; notify the parent's waitpid
 * and park on the stop queue until a SIGCONT re-readies us. */
void sched_stop_current(void) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct percpu *pc = smp_this_percpu();
    struct thread *cur = pc->current;
    if (!cur || cur == pc->idle) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }
    /* Wake the parent if it is waiting (waitpid blocks with wait_q == NULL). */
    if (cur->parent && cur->parent->state == THREAD_BLOCKED && !cur->parent->wait_q) {
        cur->parent->state = THREAD_READY;
        queue_push(&ready_queue_head, &ready_queue_tail, cur->parent);
    }
    sched_wait_locked(&stop_wq, flags);  /* releases the lock; returns on cont */
}

int sched_list_procs(struct proc_info *out, int max) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    int n = 0;
    for (struct thread *t = all_threads; t && n < max; t = t->all_next) {
        if (!t->cr3) continue;   /* user processes only */
        out[n].pid  = (int)t->tid;
        out[n].ppid = t->parent ? (int)t->parent->tid : 0;
        out[n].pgid = t->pgid;
        char st = '?';
        switch (t->state) {
            case THREAD_RUNNING: st = 'R'; break;
            case THREAD_READY:   st = 'R'; break;
            case THREAD_BLOCKED: st = t->stopped ? 'T' : 'S'; break;
            case THREAD_ZOMBIE:  st = 'Z'; break;
        }
        out[n].state = st;
        for (int i = 0; i < 31; i++) out[n].name[i] = t->name[i];
        out[n].name[31] = '\0';
        n++;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return n;
}

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
 * Release sched_lock from the freshly switched-in context.
 *
 * The CPU that performed the switch left sched_lock held and stored the
 * outgoing thread's saved RFLAGS in thread::switch_flags.  Whoever runs next on
 * this CPU (the resumed thread, or a brand-new thread entering through a
 * trampoline) restores that IRQ state and drops the lock.
 * -------------------------------------------------------------------------- */
void sched_finish_switch(void) {
    struct thread *me = smp_this_percpu()->current;
    /* Reload this thread's userspace TLS base.  Only user threads carry a TLS
     * base; the kernel locates per-CPU data via the LAPIC id, not FS, so this
     * never disturbs kernel bookkeeping.  Writing it here (rather than in the
     * asm return paths) covers fork/clone children, preemption, and ordinary
     * resume in one place. */
    if (me->cr3)
        wrmsr(MSR_FS_BASE, me->fs_base);
    spin_unlock_irqrestore(&sched_lock, me->switch_flags);
}

/* --------------------------------------------------------------------------
 * Context switch: CR3 + TSS.RSP0 + stack switch
 *
 * Entered with sched_lock held; the switched-in thread releases it via
 * sched_finish_switch().
 * -------------------------------------------------------------------------- */
static void sched_switch_context(struct thread *prev, struct thread *next) {
    /* Update this CPU's TSS RSP0 for the next thread (ring3->ring0 entry). */
    if (next->kernel_stack) {
        tss_set_rsp0(next->kernel_stack);
    } else if (next->stack_base) {
        tss_set_rsp0(next->stack_base);
    } else {
        /* Idle thread: use default kernel stack top */
        tss_set_rsp0(0x300000);
    }

    /* Switch page table if needed.  A thread with cr3 == 0 is a kernel-only
     * thread (idle, shell, kworkers) and must run in the kernel address space;
     * otherwise it would keep executing on the *previous* thread's page tables,
     * which is fatal once that process exits and its tables are freed. */
    uint64_t target_cr3 = next->cr3 ? next->cr3 : vmm_kernel_cr3();
    if (target_cr3 != vmm_get_cr3()) {
        vmm_switch_pagetable(target_cr3);
    }

    switch_thread(&prev->rsp, next->rsp);

    /* Resumed here when some CPU later switches back to `prev`.  Release the
     * scheduler lock held across that switch. */
    sched_finish_switch();
}

/* --------------------------------------------------------------------------
 * Thread trampoline
 * -------------------------------------------------------------------------- */
void thread_trampoline(void) {
    /* First instructions of a brand-new thread: the switching CPU left
     * sched_lock held, so release it before doing anything else. */
    sched_finish_switch();

    /* Re-enable interrupts: new threads start with IF=0 because they
     * were entered via switch_thread/ret rather than iretq. */
    __asm__ __volatile__("sti");
    struct thread *t = current_thread();
    if (!t) {
        thread_exit();
    }

    if (t->cr3 != 0) {
        extern void enter_userspace(uint64_t rip, uint64_t rsp);

        /* Initial process: load its program image now that we run in its own
         * address space (the scheduler already switched cr3 to t->cr3). */
        if (t->exec_path) {
            extern int elf_execve(struct thread *t, const char *path,
                                  char *const argv[], char *const envp[]);
            char *argv[] = { (char *)t->exec_path, NULL };
            int r = elf_execve(t, t->exec_path, argv, NULL);
            t->exec_path = NULL;
            if (r < 0) {
                serial_printf(SERIAL_COM1,
                    "[SCHED] initial exec failed (%d)\n", r);
                thread_exit();
            }
            enter_userspace(t->tmp_rip, t->tmp_rsp);
            /* Does not return */
        }

        /* User thread (e.g. forked child): drop to ring 3 */
        uint64_t rip = t->fork_rip ? t->fork_rip : USER_CODE_START;
        uint64_t rsp = t->fork_rsp ? t->fork_rsp : t->user_rsp;
        enter_userspace(rip, rsp);
        /* Does not return */
    }

    if (t->entry) {
        t->entry(t->arg);
    }
    thread_exit();
}

/* --------------------------------------------------------------------------
 * Thread creation (kernel thread)
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
    t->cr3 = 0;
    t->kernel_stack = 0;
    t->user_rsp = 0;
    t->switch_flags = 0x2;   /* IF=0 on first run; thread_trampoline does sti */

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

    sched_register_thread(t);
    queue_push(&ready_queue_head, &ready_queue_tail, t);

    spin_unlock_irqrestore(&sched_lock, flags);
    return t;
}

/* --------------------------------------------------------------------------
 * Thread exit
 * -------------------------------------------------------------------------- */
void thread_exit(void) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct percpu *pc = smp_this_percpu();
    struct thread *cur = pc->current;

    if (cur) {
        cur->state = THREAD_ZOMBIE;
        /* Remove from ready queue if present */
        queue_remove(&ready_queue_head, &ready_queue_tail, cur);

        /* Reparent any orphaned children to the reaper (init), so their exit
         * status can still be collected and they are not leaked. */
        if (reaper && reaper != cur && cur->children) {
            struct thread *child = cur->children;
            while (child) {
                struct thread *next_sib = child->sibling;
                child->parent = reaper;
                child->sibling = reaper->children;
                reaper->children = child;
                /* If an adopted child is already a zombie and the reaper is
                 * blocked in wait, wake it up to reap. */
                if (child->state == THREAD_ZOMBIE &&
                    reaper->state == THREAD_BLOCKED) {
                    reaper->state = THREAD_READY;
                    queue_push(&ready_queue_head, &ready_queue_tail, reaper);
                }
                child = next_sib;
            }
            cur->children = NULL;
        }

        /* Wake up parent if it's waiting for a child */
        if (cur->parent && cur->parent->state == THREAD_BLOCKED) {
            cur->parent->state = THREAD_READY;
            queue_push(&ready_queue_head, &ready_queue_tail, cur->parent);
        }
    }

    /* Pick next thread */
    struct thread *next = queue_pop(&ready_queue_head, &ready_queue_tail);
    if (!next) {
        next = pc->idle;
    }

    /* cur is a zombie and will never be resumed, so its switch_flags are
     * irrelevant; `next` releases the lock when it runs. */
    if (cur) cur->switch_flags = flags;
    next->state = THREAD_RUNNING;
    pc->current = next;

    /* Switch to next thread. We never return from this call. */
    sched_switch_context(cur, next);

    /* Should never reach here */
    __asm__ __volatile__("cli; hlt");
}

/* --------------------------------------------------------------------------
 * Thread yield
 * -------------------------------------------------------------------------- */
void thread_yield(void) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct percpu *pc = smp_this_percpu();
    struct thread *cur = pc->current;

    /* Re-enqueue the running thread, but never the idle thread: idle is only a
     * fallback when the ready queue is empty.  If it were placed in the queue it
     * would compete with real threads and, once scheduled, sit forever in its
     * hlt loop (cooperative scheduler), starving everything behind it. */
    if (cur && cur->state == THREAD_RUNNING && cur != pc->idle) {
        cur->state = THREAD_READY;
        queue_push(&ready_queue_head, &ready_queue_tail, cur);
    }

    struct thread *next = queue_pop(&ready_queue_head, &ready_queue_tail);
    if (!next) {
        next = pc->idle;
    }

    if (next == cur) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    cur->switch_flags = flags;
    next->state = THREAD_RUNNING;
    pc->current = next;

    sched_switch_context(cur, next);
}

/* --------------------------------------------------------------------------
 * Thread sleep
 * -------------------------------------------------------------------------- */
void thread_sleep(uint64_t ticks) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct percpu *pc = smp_this_percpu();
    struct thread *cur = pc->current;

    extern uint64_t timer_get_ticks(void);
    uint64_t wakeup = timer_get_ticks() + ticks;

    if (cur && cur != pc->idle) {
        cur->state = THREAD_BLOCKED;
        cur->wakeup_tick = wakeup;
        cur->next = sleep_list;
        sleep_list = cur;
    }

    struct thread *next = queue_pop(&ready_queue_head, &ready_queue_tail);
    if (!next) {
        next = pc->idle;
    }

    if (next == cur) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    cur->switch_flags = flags;
    next->state = THREAD_RUNNING;
    pc->current = next;

    sched_switch_context(cur, next);
}

/* --------------------------------------------------------------------------
 * Scheduler tick: called from timer IRQ
 * -------------------------------------------------------------------------- */
void scheduler_tick(registers_t *r) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct percpu *pc = smp_this_percpu();
    struct thread *cur = pc->current;

    /* If this CPU's scheduler isn't up yet (no idle thread claimed), bail. */
    if (!cur) {
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    (void)r;

    /* Wake up sleeping threads.  Only the BSP advances the global tick, so only
     * the BSP needs to scan the (global) sleep list; doing it on one CPU avoids
     * redundant work and keeps wakeups serialized by sched_lock. */
    if (pc->is_bsp) {
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
    }

    /* Preemptive scheduling: pick the next ready thread and context-switch to
     * it directly from the timer IRQ.  Runs on every CPU against the shared
     * ready queue, so application processors schedule ring-3 threads too.
     *
     * This is safe because:
     *   - The IRQ stub already sent the EOI before calling us, so timer IRQs
     *     keep arriving on whichever thread we switch to.
     *   - switch_thread() saves the interrupted thread's kernel RSP (which
     *     still points at the full IRQ register frame plus the C call frames)
     *     into cur->rsp.  When the thread is later resumed (possibly on another
     *     CPU), switch_thread returns back up through scheduler_tick -> the IRQ
     *     stub, which iretq's and restores the *complete* interrupted state.
     *   - sched_lock is held across switch_thread (released by the switched-in
     *     thread), so no other CPU can pop `cur` before its RSP is saved.
     *   - Syscalls run with IF masked (SFMASK), so we never preempt a thread
     *     in the middle of a syscall; only ring-3 user code and IF=1 kernel
     *     code (e.g. the idle hlt loop) get preempted.
     */
    struct thread *next = queue_pop(&ready_queue_head, &ready_queue_tail);
    if (!next) {
        /* Nothing else to run; keep running the current thread. */
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    /* Re-enqueue the outgoing thread unless it is this CPU's idle thread (idle
     * is only a fallback and must never sit in the ready queue). */
    if (cur->state == THREAD_RUNNING && cur != pc->idle) {
        cur->state = THREAD_READY;
        queue_push(&ready_queue_head, &ready_queue_tail, cur);
    }

    cur->switch_flags = flags;
    next->state = THREAD_RUNNING;
    pc->current = next;

    sched_switch_context(cur, next);
}

/* --------------------------------------------------------------------------
 * Scheduler init
 * -------------------------------------------------------------------------- */
void sched_init_cpu(void) {
    struct percpu *pc = smp_this_percpu();
    uint32_t idx = pc->cpu_index;
    if (idx >= MAX_CPUS) idx = 0;

    struct thread *idle = &idle_threads[idx];
    memset(idle, 0, sizeof(*idle));
    idle->tid = 0;
    idle->state = THREAD_RUNNING;
    idle->cr3 = 0;                 /* kernel address space */
    idle->switch_flags = 0x2;      /* IF=0 baseline */
    idle->kernel_stack = pc->stack_top;  /* 0 on the BSP, AP stack top on APs */

    pc->idle = idle;
    pc->current = idle;
}

void scheduler_init(void) {
    /* The BSP's current execution (kmain) becomes its idle thread. */
    sched_init_cpu();
    serial_printf(SERIAL_COM1,
                  "[SCHED] Scheduler initialized; BSP idle thread is kmain\n");
}

/* --------------------------------------------------------------------------
 * Add a pre-built thread to the ready queue
 * -------------------------------------------------------------------------- */
/* Assign a tid, register, optionally write the tid back to one or two user
 * addresses, then make the thread runnable - all under the scheduler lock so
 * the writes happen-before the thread can run (and, crucially, before a
 * just-spawned clone child could exit and clear the same word). */
void sched_enqueue_thread_tid(struct thread *t, int *settid_a, int *settid_b) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    t->tid = next_tid++;
    t->state = THREAD_READY;
    /* A freshly-created user process with no inherited group leads its own
     * process group and session (fork inherits the parent's via memcpy). */
    if (t->cr3 && t->pgid == 0) { t->pgid = (int)t->tid; t->sid = (int)t->tid; }
    t->next = NULL;
    t->switch_flags = 0x2;   /* IF=0 on first run; trampoline/iretq enables it */
    sched_register_thread(t);
    if (settid_a) *settid_a = (int)t->tid;
    if (settid_b) *settid_b = (int)t->tid;
    if (ready_queue_tail) {
        ready_queue_tail->next = t;
    } else {
        ready_queue_head = t;
    }
    ready_queue_tail = t;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_enqueue_thread(struct thread *t) {
    sched_enqueue_thread_tid(t, NULL, NULL);
}

/* --------------------------------------------------------------------------
 * Accessor
 * -------------------------------------------------------------------------- */
struct thread *current_thread(void) {
    return smp_this_percpu()->current;
}

/* --------------------------------------------------------------------------
 * Wait queues
 *
 * The scheduler lock doubles as the wait-queue lock.  Because it is taken with
 * interrupts masked, a producer running in an IRQ on the same CPU cannot
 * interleave between a waiter's condition check and its block, which is what
 * prevents lost wakeups (see WAIT_EVENT in sched.h).
 * -------------------------------------------------------------------------- */
uint64_t sched_lock_acquire(void) {
    return spin_lock_irqsave(&sched_lock);
}

void sched_lock_release(uint64_t flags) {
    spin_unlock_irqrestore(&sched_lock, flags);
}

void sched_wait_locked(wait_queue_t *wq, uint64_t flags) {
    struct percpu *pc = smp_this_percpu();
    struct thread *cur = pc->current;

    if (!cur || cur == pc->idle) {
        /* The idle thread must never block. */
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    cur->state = THREAD_BLOCKED;
    cur->wait_q = wq;
    queue_push(&wq->head, &wq->tail, cur);

    struct thread *next = queue_pop(&ready_queue_head, &ready_queue_tail);
    if (!next) next = pc->idle;

    if (next == cur) {
        /* Nothing else to run: undo the block and spin-yield via idle is not
         * possible here, so just unblock and return (caller re-checks). */
        queue_remove(&wq->head, &wq->tail, cur);
        cur->wait_q = NULL;
        cur->state = THREAD_RUNNING;
        spin_unlock_irqrestore(&sched_lock, flags);
        return;
    }

    cur->switch_flags = flags;
    next->state = THREAD_RUNNING;
    pc->current = next;
    sched_switch_context(cur, next);
    /* Resumed here after a wq_wake_*; sched_lock already released. */
    cur->wait_q = NULL;
}

void wq_wake_one(wait_queue_t *wq) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct thread *t = queue_pop(&wq->head, &wq->tail);
    if (t) {
        t->wait_q = NULL;
        t->state = THREAD_READY;
        queue_push(&ready_queue_head, &ready_queue_tail, t);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

int sched_wq_wake_n(wait_queue_t *wq, int n) {
    int woke = 0;
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct thread *t;
    while (woke < n && (t = queue_pop(&wq->head, &wq->tail))) {
        t->wait_q = NULL;
        t->state = THREAD_READY;
        queue_push(&ready_queue_head, &ready_queue_tail, t);
        woke++;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return woke;
}

void wq_wake_all(wait_queue_t *wq) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    struct thread *t;
    while ((t = queue_pop(&wq->head, &wq->tail))) {
        t->wait_q = NULL;
        t->state = THREAD_READY;
        queue_push(&ready_queue_head, &ready_queue_tail, t);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

/* Wake one specific thread from whatever wait queue it is blocked on. */
void sched_wake_thread(struct thread *t) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (t && t->state == THREAD_BLOCKED && t->wait_q) {
        queue_remove(&t->wait_q->head, &t->wait_q->tail, t);
        t->wait_q = NULL;
        t->state = THREAD_READY;
        queue_push(&ready_queue_head, &ready_queue_tail, t);
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

int sched_signal_pending(void) {
    struct thread *t = current_thread();
    return t && ((t->sig_pending & ~t->sig_mask) != 0);
}
