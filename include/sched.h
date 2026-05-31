#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>
#include "idt.h"
#include "fd.h"

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
    uint64_t          stack_base;   /* top of allocated stack (for freeing) */
    uint64_t          stack_size;
    void            (*entry)(void *arg);
    void             *arg;
    uint64_t          wakeup_tick;  /* for sleep */
    struct thread    *next;

    /* Userspace fields (valid for user threads) */
    uint64_t          cr3;          /* page table physical address */
    uint64_t          kernel_stack; /* top of kernel stack for syscall/irq */
    uint64_t          user_rsp;     /* userspace stack pointer */
    struct fd_table  *fdt;          /* file descriptor table */

    /* Process hierarchy (for fork/wait) */
    struct thread    *parent;       /* parent process */
    struct thread    *children;     /* list of child processes */
    struct thread    *sibling;      /* next child of the same parent */
    int               exit_code;    /* exit status (valid if ZOMBIE) */
    int               waited;       /* parent has already waited for this child */

    /* Saved syscall state (used by fork to return child to userspace) */
    uint64_t          fork_rip;
    uint64_t          fork_rflags;
    uint64_t          fork_rsp;
    uint64_t          fork_rax;

    /* Per-thread syscall entry state (not global) */
    uint64_t          tmp_rip;
    uint64_t          tmp_rflags;
    uint64_t          tmp_rsp;

    /* Syscall number saved by syscall_entry (per-thread, not global) */
    uint64_t          syscall_nr;

    /* Full user register snapshot (saved on syscall entry) */
    uint64_t          usr_rdi;
    uint64_t          usr_rsi;
    uint64_t          usr_rdx;
    uint64_t          usr_r10;
    uint64_t          usr_r8;
    uint64_t          usr_r9;
    uint64_t          usr_rbx;
    uint64_t          usr_rbp;
    uint64_t          usr_r12;
    uint64_t          usr_r13;
    uint64_t          usr_r14;
    uint64_t          usr_r15;

    /* --- Fields below are NOT referenced by syscall_asm.asm offsets, so they
     * must remain AFTER usr_r15 to keep those hardcoded offsets valid. --- */

    /* RFLAGS to restore when this thread is next resumed.  The scheduler holds
     * sched_lock across the context switch and the thread that gets switched IN
     * releases it (see sched_finish_switch), restoring the IRQ state that was
     * in effect when this thread was last switched OUT. */
    uint64_t          switch_flags;

    /* User heap (brk) and anonymous mmap arena */
    uint64_t          brk_start;
    uint64_t          brk_cur;
    uint64_t          mmap_next;

    /* Program to load on first dispatch (initial process only); NULL once the
     * thread is running its image.  Used by thread_trampoline to load an ELF
     * inside the thread's own address space via elf_execve. */
    const char       *exec_path;

    /* Current working directory (absolute, normalized).  Inherited across
     * fork/exec; used by getcwd/chdir. */
    char              cwd[256];

    /* Signals: handler table, pending and blocked masks */
    uint64_t          sig_handlers[32];
    uint64_t          sig_pending;   /* bitmask of pending signals */
    uint64_t          sig_mask;      /* bitmask of blocked signals */
    uint64_t          sig_restorer;  /* user trampoline that calls sigreturn */

    /* Process groups / sessions (job control + terminal signals). */
    int               pgid;          /* process group id */
    int               sid;           /* session id */

    /* Job control: stopped state (SIGTSTP/SIGSTOP) and waitpid reporting. */
    int               stopped;       /* non-zero while job-control stopped */
    int               stop_reported; /* waitpid(WUNTRACED) already saw the stop */
    int               stop_sig;      /* signal that caused the stop */

    /* Short program name (basename of the exec path) for ps. */
    char              name[32];

    /* Wait queue this thread is currently blocked on (for signal wakeups). */
    struct wait_queue *wait_q;

    /* Global thread registry (for kill/find-by-tid) */
    struct thread    *all_next;

    /* --- M9: threads (clone/futex/TLS) ---------------------------------- */
    /* Userspace TLS base, programmed into IA32_FS_BASE on every switch into
     * this thread (arch_prctl(ARCH_SET_FS) / CLONE_SETTLS). */
    uint64_t          fs_base;

    /* Shared address-space reference count.  NULL means this thread solely
     * owns its cr3 (the common case); when several CLONE_VM threads share one
     * address space they point at a single heap-allocated counter and the cr3
     * is destroyed only when it reaches zero. */
    int              *mm_count;

    /* CLONE_CHILD_CLEARTID: on exit the kernel zeroes *clear_child_tid in the
     * (shared) address space and futex-wakes it, which is how pthread_join
     * observes thread completion.  0 when unused. */
    uint64_t          clear_child_tid;

    /* --- M10: multi-user credentials -----------------------------------
     * Real/effective/saved user and group ids plus supplementary groups.
     * Inherited across fork/clone (memcpy) and preserved over exec except for
     * the set-user-ID bit.  uid 0 is root and bypasses permission checks. */
    uint32_t          uid, euid, suid;
    uint32_t          gid, egid, sgid;
#define NGROUPS_MAX 16
    uint32_t          groups[NGROUPS_MAX];
    int               ngroups;

    /* File-mode creation mask (umask). Inherited across fork/clone (memcpy)
     * and preserved over exec; cleared bits in the mask are removed from the
     * permission bits of newly created files/dirs. */
    uint32_t          umask;

    /* Thread-group id == POSIX process id.  All threads of a process (clone with
     * CLONE_THREAD) share one tgid; getpid() returns this, gettid() returns the
     * per-thread tid.  A fresh process has tgid == tid (set at enqueue). */
    uint32_t          tgid;

    /* Per-thread blocked signal mask managed by rt_sigprocmask (distinct from
     * sig_mask which is also used transiently during signal delivery). */

    /* User GS base set via arch_prctl(ARCH_SET_GS) (rarely used; not restored on
     * context switch - documented limitation). */
    uint64_t          gs_base;
};

/* --------------------------------------------------------------------------
 * Scheduler API
 * -------------------------------------------------------------------------- */
void scheduler_init(void);

/* Per-CPU scheduler bring-up: claim this CPU's idle thread and make it the
 * current thread.  Called once per CPU (BSP via scheduler_init, each AP from
 * ap_main) before that CPU's LAPIC timer starts preempting. */
void sched_init_cpu(void);

/* Release sched_lock from the context that was just switched in.  Called from
 * the C resume path and from the assembly thread entry trampolines
 * (thread_trampoline, fork_return_asm). */
void sched_finish_switch(void);

struct thread *thread_create(void (*entry)(void *), void *arg);
void thread_yield(void);
void thread_sleep(uint64_t ticks);
void thread_exit(void);

struct thread *current_thread(void);

/* Add a pre-built thread to the ready queue */
void sched_enqueue_thread(struct thread *t);

/* Like sched_enqueue_thread, but writes the freshly-assigned tid to up to two
 * user addresses under the scheduler lock (clone CLONE_*_SETTID). */
void sched_enqueue_thread_tid(struct thread *t, int *settid_a, int *settid_b);

/* Designate the thread that adopts orphaned children (init / PID 1). */
void sched_set_reaper(struct thread *t);

/* Thread registry lookup / removal (for kill, waitpid reaping). */
struct thread *sched_find_by_tid(uint32_t tid);
void sched_remove_thread(struct thread *t);

/* Set `sig` pending on all user threads in process group `pgid`. */
int sched_signal_pgrp(int pgid, int sig);

/* Deliver `sig` to a single thread with job-control semantics (SIGCONT resumes
 * a stopped thread, other signals wake a blocked one). */
void sched_deliver_signal(struct thread *t, int sig);

/* Stop the current thread (job-control SIGTSTP/SIGSTOP); returns on SIGCONT. */
void sched_stop_current(void);

/* Snapshot of live processes for ps(1). */
struct proc_info {
    int  pid;
    int  ppid;
    int  pgid;
    char state;     /* R/S/T/Z */
    char name[32];
};
int sched_list_procs(struct proc_info *out, int max);

/* Called from timer IRQ with saved register frame */
void scheduler_tick(registers_t *r);

/* Assembly helper: switch stacks */
extern void switch_thread(uint64_t *old_rsp, uint64_t new_rsp);

/* --------------------------------------------------------------------------
 * Wait queues
 *
 * A wait queue lets a thread block until some event wakes it, instead of busy
 * spinning.  The canonical usage avoids lost-wakeups by checking the condition
 * with the scheduler lock held (interrupts are masked while it is held, so a
 * same-CPU IRQ producer cannot interleave):
 *
 *     for (;;) {
 *         uint64_t f = sched_lock_acquire();
 *         if (CONDITION) { sched_lock_release(f); break; }
 *         sched_wait_locked(&wq, f);   // blocks; re-acquires nothing
 *     }
 *
 * or simply WAIT_EVENT(wq, CONDITION).  Producers set the condition and then
 * call wq_wake_one()/wq_wake_all().
 * -------------------------------------------------------------------------- */
typedef struct wait_queue {
    struct thread *head;
    struct thread *tail;
} wait_queue_t;

#define WAIT_QUEUE_INIT { NULL, NULL }

uint64_t sched_lock_acquire(void);
void     sched_lock_release(uint64_t flags);

/* Block the current thread on `wq`.  Must be called with the scheduler lock
 * held (flags from sched_lock_acquire); returns after being woken with the lock
 * released. */
void sched_wait_locked(wait_queue_t *wq, uint64_t flags);

void wq_wake_one(wait_queue_t *wq);
void wq_wake_all(wait_queue_t *wq);

/* Wake up to `n` threads blocked on `wq`; returns how many were woken.  Used by
 * the futex FUTEX_WAKE path. */
int  sched_wq_wake_n(wait_queue_t *wq, int n);

/* Wake a specific thread from whatever wait queue it is blocked on (used by
 * signal delivery so a blocked syscall can return -EINTR). */
void sched_wake_thread(struct thread *t);

/* Non-zero if the current thread has a pending, unblocked signal. */
int sched_signal_pending(void);

#define WAIT_EVENT(wq, cond)                                  \
    do {                                                      \
        for (;;) {                                            \
            uint64_t __we_f = sched_lock_acquire();           \
            if (cond) { sched_lock_release(__we_f); break; }  \
            sched_wait_locked(&(wq), __we_f);                 \
        }                                                     \
    } while (0)

/* Like WAIT_EVENT, but also returns when a signal is pending; sets `intr` to 1
 * in that case (and 0 otherwise). */
#define WAIT_EVENT_INTR(wq, cond, intr)                       \
    do {                                                      \
        (intr) = 0;                                           \
        for (;;) {                                            \
            uint64_t __we_f = sched_lock_acquire();           \
            if (cond) { sched_lock_release(__we_f); break; }  \
            if (sched_signal_pending()) {                     \
                sched_lock_release(__we_f); (intr) = 1; break;\
            }                                                 \
            sched_wait_locked(&(wq), __we_f);                 \
        }                                                     \
    } while (0)

#endif /* SCHED_H */
