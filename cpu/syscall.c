#include "syscall.h"
#include "msr.h"
#include "gdt.h"
#include "serial.h"
#include "vga.h"
#include "sched.h"
#include "kapi.h"
#include "vfs.h"
#include "fd.h"
#include "socket.h"
#include "keyboard.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "errno.h"
#include "uapi.h"
#include "smp.h"
#include "timer.h"
#include "ipc.h"
#include "procfs.h"
#include <string.h>

/* MSR addresses */
#define MSR_EFER     0xC0000080
#define MSR_STAR     0xC0000081
#define MSR_LSTAR    0xC0000082
#define MSR_SFMASK   0xC0000084

#define EFER_SCE     (1ULL << 0)
#define RFLAGS_IF    (1ULL << 9)

extern void syscall_entry(void);

typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static syscall_fn_t syscall_table[MAX_SYSCALLS];

/* User heap/mmap arena defaults (chosen to avoid colliding with code @1GB and
 * the stack @3GB). */
#define USER_HEAP_BASE   0x0000000050000000ULL
#define USER_MMAP_BASE   0x0000000070000000ULL

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
static uint64_t *cur_pml4(struct thread *t) {
    return (uint64_t *)phys_to_virt(t->cr3);
}

/* Map (and zero) a single anonymous user page into the process. */
static int map_user_page(struct thread *t, uint64_t virt, uint64_t flags) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return -ENOMEM;
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    if (vmm_map_page_in(cur_pml4(t), virt, phys,
                        PT_USER | PT_PRESENT | flags) < 0) {
        pmm_free_page(phys);
        return -ENOMEM;
    }
    return 0;
}

/* Resolve a possibly-relative user path against the calling thread's current
 * working directory.  Absolute paths are returned unchanged; relative paths are
 * joined onto cwd into `buf` (vfs_walk() collapses any "."/".." components).
 * The VFS only accepts absolute paths, so every path syscall must funnel user
 * input through here or relative paths (the norm for builds: `cc -c main.c`)
 * silently fail.  Returns NULL on a NULL input or buffer overflow. */
static const char *cwd_join(const char *p, char *buf, size_t sz) {
    if (!p) return NULL;
    if (p[0] == '/') return p;
    struct thread *t = current_thread();
    const char *cwd = (t && t->cwd[0]) ? t->cwd : "/";
    size_t cl = strlen(cwd), pl = strlen(p), off;
    if (cl + 1 + pl + 1 > sz) return NULL;
    memcpy(buf, cwd, cl); off = cl;
    if (off == 0 || buf[off - 1] != '/') buf[off++] = '/';
    memcpy(buf + off, p, pl); off += pl;
    buf[off] = '\0';
    return buf;
}

/* --------------------------------------------------------------------------
 * Process / file syscalls
 * -------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------
 * Futex (M9): a small hash of wait queues keyed by the futex's userspace
 * virtual address.  Threads sharing an address space (CLONE_VM) see the same
 * address, so hashing on the virtual address is sufficient for pthreads.
 * -------------------------------------------------------------------------- */
#define FUTEX_NBUCKETS 64
static wait_queue_t futex_buckets[FUTEX_NBUCKETS];

#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_WAIT_BITSET   9
#define FUTEX_WAKE_BITSET   10
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_REALTIME 256

static wait_queue_t *futex_bucket(uint64_t uaddr) {
    return &futex_buckets[(uaddr >> 3) % FUTEX_NBUCKETS];
}

/* Wake any thread waiting on `uaddr` (used by CLONE_CHILD_CLEARTID on exit). */
static void futex_wake_addr(uint64_t uaddr, int n) {
    sched_wq_wake_n(futex_bucket(uaddr), n);
}

static uint64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                          uint64_t timeout, uint64_t uaddr2, uint64_t val3) {
    (void)timeout; (void)uaddr2; (void)val3;
    int cmd = (int)(op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME));
    int *addr = (int *)(uintptr_t)uaddr;
    if (!addr) return -EINVAL;

    /* Treat the *_BITSET variants like their plain counterparts: we ignore the
     * bitset (val3), which is a valid simplification for pthread/musl use. */
    if (cmd == FUTEX_WAIT_BITSET) cmd = FUTEX_WAIT;
    else if (cmd == FUTEX_WAKE_BITSET) cmd = FUTEX_WAKE;

    switch (cmd) {
    case FUTEX_WAIT: {
        wait_queue_t *wq = futex_bucket(uaddr);
        uint64_t f = sched_lock_acquire();
        if ((uint32_t)*addr != (uint32_t)val) {
            sched_lock_release(f);
            return -EAGAIN;
        }
        if (sched_signal_pending()) {
            sched_lock_release(f);
            return -EINTR;
        }
        /* Blocks and releases the lock; returns once woken (spurious wakeups are
         * allowed - the userspace caller re-checks the condition). */
        sched_wait_locked(wq, f);
        return sched_signal_pending() ? -EINTR : 0;
    }
    case FUTEX_WAKE:
        return (uint64_t)sched_wq_wake_n(futex_bucket(uaddr), (int)val);
    default:
        return -ENOSYS;
    }
}

/* On thread exit, satisfy a pthread_join blocked via CLONE_CHILD_CLEARTID:
 * zero the futex word in the (still-mapped) address space and wake waiters. */
static void clear_child_tid_on_exit(struct thread *t) {
    if (t && t->clear_child_tid) {
        int *p = (int *)(uintptr_t)t->clear_child_tid;
        *p = 0;
        futex_wake_addr(t->clear_child_tid, 0x7fffffff);
        t->clear_child_tid = 0;
    }
}

static uint64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (t) {
        t->exit_code = (int)code & 0xff;
        clear_child_tid_on_exit(t);
    }
    thread_exit();
    return 0; /* never reached */
}

static uint64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t ptid,
                          uint64_t ctid, uint64_t tls, uint64_t a6) {
    (void)a6;
    struct thread *parent = current_thread();
    struct thread *child = process_clone(parent, flags, stack, tls,
                                         (int *)(uintptr_t)ptid,
                                         (int *)(uintptr_t)ctid);
    if (!child) return -EAGAIN;
    return child->tid;
}

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_SET_GS 0x1001
#define ARCH_GET_GS 0x1004

static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t) return -EINVAL;
    switch (code) {
    case ARCH_SET_FS:
        t->fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    case ARCH_GET_FS:
        if (!addr) return -EINVAL;
        *(uint64_t *)(uintptr_t)addr = t->fs_base;
        return 0;
    case ARCH_SET_GS:
        /* User GS base. Programmed immediately; not restored on context switch
         * (rarely used by userland - documented in sched.h). */
        t->gs_base = addr;
        wrmsr(MSR_GS_BASE, addr);
        return 0;
    case ARCH_GET_GS:
        if (!addr) return -EINVAL;
        *(uint64_t *)(uintptr_t)addr = t->gs_base;
        return 0;
    default:
        return -EINVAL;
    }
}

static uint64_t sys_set_tid_address(uint64_t tidptr, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t) return -EINVAL;
    t->clear_child_tid = tidptr;
    return t->tid;
}

static uint64_t sys_gettid(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    return t ? t->tid : 0;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    const char *str = (const char *)(uintptr_t)buf;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    ssize_t n = vfs_write(t->fdt->files[fd], str, count);
    return (n < 0) ? (uint64_t)-EIO : (uint64_t)n;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char *dest = (char *)(uintptr_t)buf;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    ssize_t n = vfs_read(t->fdt->files[fd], dest, count);
    if (n == -1) return (uint64_t)-EIO;          /* generic failure */
    return (uint64_t)n;                          /* >=0, or a real -errno */
}

static uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6; (void)mode;
    struct thread *t = current_thread();
    if (!t || !t->fdt) return -EINVAL;
    char pbuf[1024];
    const char *p = cwd_join((const char *)(uintptr_t)path, pbuf, sizeof pbuf);
    if (!p) return -ENOENT;
    int perr = vfs_access_check(p, (int)flags);
    if (perr == -EACCES) return -EACCES;
    int existed = ((flags & O_CREAT) && vfs_lookup_path(p) != NULL);
    struct vfs_file *file = vfs_open(p, (int)flags);
    if (!file) return -ENOENT;
    /* A newly O_CREAT'd file is owned by the creating user. */
    if ((flags & O_CREAT) && !existed && file->inode) {
        file->inode->uid = t->euid;
        file->inode->gid = t->egid;
        vfs_setattr(file->inode);
    }
    int fd = fd_alloc(t->fdt, file);
    if (fd < 0) {
        vfs_close(file);
        return -EMFILE;
    }
    return (uint64_t)fd;
}

static uint64_t sys_close(uint64_t fd, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX) return -EBADF;
    if (!t->fdt->files[fd]) return -EBADF;
    fd_close(t->fdt, (int)fd);
    return 0;
}

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    return (uint64_t)vfs_lseek(t->fdt->files[fd], (off_t)offset, (int)whence);
}

static uint64_t sys_dup(uint64_t oldfd, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt) return -EBADF;
    int r = fd_dup(t->fdt, (int)oldfd);
    return (r < 0) ? (uint64_t)-EBADF : (uint64_t)r;
}

static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt) return -EBADF;
    int r = fd_dup2(t->fdt, (int)oldfd, (int)newfd);
    return (r < 0) ? (uint64_t)-EBADF : (uint64_t)r;
}

static uint64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    /* POSIX getpid() returns the process (thread-group) id, not the tid. */
    return t ? (uint64_t)(t->tgid ? t->tgid : t->tid) : 0;
}

static uint64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->parent) return 0;
    struct thread *p = t->parent;
    return (uint64_t)(p->tgid ? p->tgid : p->tid);
}

static uint64_t sys_yield(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    thread_yield();
    return 0;
}

static uint64_t sys_nanosleep(uint64_t req, uint64_t rem, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    const struct timespec *ts = (const struct timespec *)(uintptr_t)req;
    struct timespec *remp = (struct timespec *)(uintptr_t)rem;
    if (!ts) return -EINVAL;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)
        return -EINVAL;

    /* Sleep based on the monotonic clock so we never wake early.  The tick is
     * 10ms; round the request up to whole ticks and re-check the deadline. */
    uint64_t want_ns = (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
    uint64_t deadline = clock_monotonic_ns() + want_ns;
    const uint64_t tick_ns = 1000000000ull / TIMER_HZ;
    for (;;) {
        uint64_t now = clock_monotonic_ns();
        if (now >= deadline) break;
        /* Interruptible: a pending unblocked signal returns -EINTR with the
         * remaining time, so terminal signals (Ctrl-C/Ctrl-Z) act promptly. */
        if (sched_signal_pending()) {
            if (remp) {
                uint64_t left = deadline - now;
                remp->tv_sec = (int64_t)(left / 1000000000ull);
                remp->tv_nsec = (int64_t)(left % 1000000000ull);
            }
            return -EINTR;
        }
        uint64_t left = deadline - now;
        uint64_t ticks = (left + tick_ns - 1) / tick_ns;
        if (ticks == 0) ticks = 1;
        if (ticks > 5) ticks = 5;   /* re-check signals at least every ~50ms */
        thread_sleep(ticks);
    }
    if (remp) { remp->tv_sec = 0; remp->tv_nsec = 0; }
    return 0;
}

/* --------------------------------------------------------------------------
 * Time syscalls
 * -------------------------------------------------------------------------- */
static uint64_t sys_clock_gettime(uint64_t clk, uint64_t tp, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct timespec *ts = (struct timespec *)(uintptr_t)tp;
    if (!ts) return -EFAULT;
    /* Only the wall clock uses realtime; MONOTONIC and the per-process /
     * per-thread CPU-time clocks (2,3) all map to the monotonic source so
     * timing deltas (e.g. GCC's timevar) are always non-decreasing. */
    uint64_t ns = (clk == CLOCK_REALTIME) ? clock_realtime_ns()
                                          : clock_monotonic_ns();
    ts->tv_sec = (int64_t)(ns / 1000000000ull);
    ts->tv_nsec = (int64_t)(ns % 1000000000ull);
    return 0;
}

static uint64_t sys_gettimeofday(uint64_t tv, uint64_t tz, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)tz; (void)a3; (void)a4; (void)a5; (void)a6;
    struct timeval *t = (struct timeval *)(uintptr_t)tv;
    if (!t) return -EFAULT;
    uint64_t ns = clock_realtime_ns();
    t->tv_sec = (int64_t)(ns / 1000000000ull);
    t->tv_usec = (int64_t)((ns % 1000000000ull) / 1000ull);
    return 0;
}

/* --------------------------------------------------------------------------
 * poll(2) / select(2)
 *
 * Both are implemented as a re-check loop with a 10ms granularity using the
 * monotonic clock for the deadline.  This keeps the kernel simple while still
 * letting servers and the shell multiplex console / pipe / socket fds.
 * -------------------------------------------------------------------------- */
static uint64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    struct pollfd *pf = (struct pollfd *)(uintptr_t)fds;
    int n = (int)nfds;
    int to = (int)(int64_t)timeout;   /* ms; <0 = infinite */
    if (!t || !t->fdt || (n > 0 && !pf)) return -EINVAL;

    uint64_t deadline = 0;
    if (to >= 0) deadline = clock_monotonic_ns() + (uint64_t)to * 1000000ull;

    for (;;) {
        int ready = 0;
        for (int i = 0; i < n; i++) {
            pf[i].revents = 0;
            int fd = pf[i].fd;
            if (fd < 0) continue;
            if (fd >= FD_MAX || !t->fdt->files[fd]) {
                pf[i].revents = POLLNVAL;
                ready++;
                continue;
            }
            short ev = (short)(pf[i].events | POLLERR | POLLHUP);
            short got = vfs_poll(t->fdt->files[fd], ev);
            if (got) { pf[i].revents = got; ready++; }
        }
        if (ready) return (uint64_t)ready;
        if (to == 0) return 0;
        if (to > 0 && clock_monotonic_ns() >= deadline) return 0;
        thread_sleep(1);
    }
}

/* fd_set is an array of 64-bit words (we cap at FD_MAX bits). */
#define FDSET_WORDS (FD_MAX / 64)
static int fdset_test(const uint64_t *set, int fd) {
    return set && (set[fd / 64] & (1ull << (fd % 64)));
}
static void fdset_set(uint64_t *set, int fd) {
    if (set) set[fd / 64] |= (1ull << (fd % 64));
}

static uint64_t sys_select(uint64_t nfds, uint64_t rfds, uint64_t wfds,
                           uint64_t efds, uint64_t timeout, uint64_t a6) {
    (void)a6;
    struct thread *t = current_thread();
    int n = (int)nfds;
    if (!t || !t->fdt || n < 0 || n > FD_MAX) return -EINVAL;
    uint64_t *rin = (uint64_t *)(uintptr_t)rfds;
    uint64_t *win = (uint64_t *)(uintptr_t)wfds;
    uint64_t *ein = (uint64_t *)(uintptr_t)efds;
    struct timeval *tv = (struct timeval *)(uintptr_t)timeout;

    int infinite = (tv == NULL);
    uint64_t deadline = 0;
    if (!infinite)
        deadline = clock_monotonic_ns() +
                   (uint64_t)tv->tv_sec * 1000000000ull +
                   (uint64_t)tv->tv_usec * 1000ull;

    uint64_t rout[FDSET_WORDS], wout[FDSET_WORDS], eout[FDSET_WORDS];
    for (;;) {
        memset(rout, 0, sizeof(rout));
        memset(wout, 0, sizeof(wout));
        memset(eout, 0, sizeof(eout));
        int ready = 0;
        for (int fd = 0; fd < n; fd++) {
            int wr = fdset_test(rin, fd), ww = fdset_test(win, fd),
                we = fdset_test(ein, fd);
            if (!wr && !ww && !we) continue;
            struct vfs_file *f = t->fdt->files[fd];
            if (!f) continue;   /* bad fd in set: treat as not ready */
            short ev = 0;
            if (wr) ev |= POLLIN;
            if (ww) ev |= POLLOUT;
            short got = vfs_poll(f, (short)(ev | POLLERR | POLLHUP));
            if (wr && (got & (POLLIN | POLLHUP))) { fdset_set(rout, fd); ready++; }
            if (ww && (got & POLLOUT))            { fdset_set(wout, fd); ready++; }
            if (we && (got & POLLERR))            { fdset_set(eout, fd); ready++; }
        }
        if (ready) {
            if (rin) memcpy(rin, rout, sizeof(rout));
            if (win) memcpy(win, wout, sizeof(wout));
            if (ein) memcpy(ein, eout, sizeof(eout));
            return (uint64_t)ready;
        }
        if (!infinite && clock_monotonic_ns() >= deadline) {
            if (rin) memset(rin, 0, sizeof(rout));
            if (win) memset(win, 0, sizeof(wout));
            if (ein) memset(ein, 0, sizeof(eout));
            return 0;
        }
        thread_sleep(1);
    }
}

static uint64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *parent = current_thread();
    struct thread *child = process_fork(parent);
    if (!child) return -EAGAIN;
    return child->tid;
}

static uint64_t sys_waitpid(uint64_t pid, uint64_t status, uint64_t options,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *parent = current_thread();
    if (!parent) return -ECHILD;

    int target_pid = (int)pid;
    int *status_ptr = (int *)(uintptr_t)status;

    for (;;) {
        int have_children = 0;
        struct thread *child = parent->children;
        while (child) {
            if (target_pid == -1 || (int)child->tid == target_pid) {
                have_children = 1;
                /* WUNTRACED (option bit 2): report a newly-stopped child. */
                if ((options & 2) && child->stopped && !child->stop_reported) {
                    child->stop_reported = 1;
                    if (status_ptr)
                        *status_ptr = ((child->stop_sig & 0xff) << 8) | 0x7f;
                    return (uint64_t)child->tid;
                }
                if (child->state == THREAD_ZOMBIE && !child->waited) {
                    child->waited = 1;
                    if (status_ptr) *status_ptr = (child->exit_code & 0xff) << 8;
                    int ret = (int)child->tid;
                    struct thread **pp = &parent->children;
                    while (*pp) {
                        if (*pp == child) { *pp = child->sibling; break; }
                        pp = &(*pp)->sibling;
                    }
                    sched_remove_thread(child);
                    fd_table_free(child->fdt);
                    /* Tear down the address space, honouring CLONE_VM sharing:
                     * a shared cr3 is only destroyed when the last thread that
                     * references it is reaped. */
                    if (child->cr3) {
                        if (child->mm_count) {
                            if (--(*child->mm_count) == 0) {
                                kfree(child->mm_count);
                                vmm_destroy_pagetable(child->cr3);
                            }
                        } else {
                            vmm_destroy_pagetable(child->cr3);
                        }
                    }
                    if (child->stack_base)
                        free_pages((void *)(child->stack_base - child->stack_size),
                                   child->stack_size / PAGE_SIZE);
                    kfree(child);
                    return (uint64_t)ret;
                }
            }
            child = child->sibling;
        }

        if (!have_children) return -ECHILD;
        if (options & 1) return 0;  /* WNOHANG */

        parent->state = THREAD_BLOCKED;
        thread_yield();
    }
}

static uint64_t sys_execve(uint64_t pathname, uint64_t argv, uint64_t envp,
                           uint64_t a4, uint64_t a5, uint64_t a6);

/* --------------------------------------------------------------------------
 * Memory syscalls
 * -------------------------------------------------------------------------- */
static uint64_t sys_brk(uint64_t addr, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->cr3) return -ENOMEM;

    if (t->brk_start == 0) {
        t->brk_start = USER_HEAP_BASE;
        t->brk_cur = USER_HEAP_BASE;
    }
    if (addr == 0 || addr < t->brk_start) {
        return t->brk_cur;  /* query */
    }

    uint64_t old = t->brk_cur;
    uint64_t new_brk = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t cur = (old + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    if (new_brk > cur) {
        for (uint64_t v = cur; v < new_brk; v += PAGE_SIZE) {
            if (map_user_page(t, v, PT_WRITABLE) < 0) return t->brk_cur;
        }
    } else if (new_brk < cur) {
        for (uint64_t v = new_brk; v < cur; v += PAGE_SIZE) {
            vmm_unmap_page_in(cur_pml4(t), v);
        }
    }
    t->brk_cur = addr;
    return t->brk_cur;
}

static uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t offset) {
    struct thread *t = current_thread();
    if (!t || !t->cr3) return (uint64_t)-ENOMEM;
    if (length == 0) return (uint64_t)-EINVAL;

    if (t->mmap_next == 0) t->mmap_next = USER_MMAP_BASE;

    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t pflags = (prot & PROT_WRITE) ? PT_WRITABLE : 0;

    /* MAP_FIXED honors the requested page-aligned address; otherwise we bump the
     * per-process arena. */
    uint64_t base;
    if ((flags & MAP_FIXED) && addr) {
        if (addr & (PAGE_SIZE - 1)) return (uint64_t)-EINVAL;
        base = addr;
    } else {
        base = t->mmap_next;
    }

    /* File-backed mapping. */
    if (!(flags & MAP_ANONYMOUS)) {
        if (!t->fdt || (int)fd < 0 || fd >= FD_MAX) return (uint64_t)-EBADF;
        struct vfs_file *f = t->fdt->files[fd];
        if (!f || !f->inode) return (uint64_t)-EBADF;

        /* Device-provided mmap (e.g. /dev/fb0) maps its own physical pages. */
        if (f->inode->f_ops && f->inode->f_ops->mmap) {
            int r = f->inode->f_ops->mmap(f, base, length, prot);
            if (r < 0) return (uint64_t)(int64_t)r;
            if (base + pages * PAGE_SIZE > t->mmap_next)
                t->mmap_next = base + pages * PAGE_SIZE;
            return base;
        }

        /* Generic regular-file mapping: back it with anonymous pages and read
         * the file contents at `offset` into them (private, copy-in). This is
         * enough for the dynamic linker and programs that mmap data files. */
        for (uint64_t i = 0; i < pages; i++) {
            if (map_user_page(t, base + i * PAGE_SIZE, PT_WRITABLE) < 0) {
                for (uint64_t j = 0; j < i; j++)
                    vmm_unmap_page_in(cur_pml4(t), base + j * PAGE_SIZE);
                return (uint64_t)-ENOMEM;
            }
        }
        off_t saved = vfs_lseek(f, 0, SEEK_CUR);
        vfs_lseek(f, (off_t)offset, SEEK_SET);
        vfs_read(f, (void *)(uintptr_t)base, length);
        vfs_lseek(f, saved, SEEK_SET);
        if (base + pages * PAGE_SIZE > t->mmap_next)
            t->mmap_next = base + pages * PAGE_SIZE;
        return base;
    }

    /* Anonymous mapping. */
    for (uint64_t i = 0; i < pages; i++) {
        if (map_user_page(t, base + i * PAGE_SIZE, pflags) < 0) {
            for (uint64_t j = 0; j < i; j++)
                vmm_unmap_page_in(cur_pml4(t), base + j * PAGE_SIZE);
            return (uint64_t)-ENOMEM;
        }
    }
    if (base + pages * PAGE_SIZE > t->mmap_next)
        t->mmap_next = base + pages * PAGE_SIZE;
    return base;
}

static uint64_t sys_munmap(uint64_t addr, uint64_t length, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->cr3) return -EINVAL;
    if (length == 0 || (addr & (PAGE_SIZE - 1))) return -EINVAL;
    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++)
        vmm_unmap_page_in(cur_pml4(t), addr + i * PAGE_SIZE);
    return 0;
}

static uint64_t sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->cr3) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    /* Best-effort: re-map each present page in the range with the requested
     * writability. Pages stay user/present; PROT_NONE is treated as read-only.
     * This satisfies the dynamic linker's RELRO and JIT-style transitions. */
    uint64_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t want_w = (prot & PROT_WRITE) ? PT_WRITABLE : 0;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t v = addr + i * PAGE_SIZE;
        uint64_t phys = vmm_virt_to_phys_in(cur_pml4(t), v);
        if (!phys) continue;   /* unmapped page in range: ignore */
        vmm_map_page_in(cur_pml4(t), v, phys,
                        PT_USER | PT_PRESENT | want_w);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Filesystem metadata syscalls
 * -------------------------------------------------------------------------- */
static void fill_stat(struct vfs_inode *inode, struct kstat *st) {
    memset(st, 0, sizeof(*st));
    st->st_dev     = 1;
    st->st_ino     = inode->inode_no;
    st->st_nlink   = inode->nlink ? inode->nlink : 1;
    st->st_mode    = inode->mode;
    st->st_blksize = 512;
    st->st_size    = inode->size;
    st->st_blocks  = (inode->size + 511) / 512;
}

static uint64_t sys_stat(uint64_t path, uint64_t statbuf, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct kstat *st = (struct kstat *)(uintptr_t)statbuf;
    if (!st) return -EFAULT;
    char pbuf[1024];
    const char *p = cwd_join((const char *)(uintptr_t)path, pbuf, sizeof pbuf);
    if (!p) return -EFAULT;
    struct vfs_dentry *d = vfs_lookup_path(p);
    if (!d || !d->inode) return -ENOENT;
    fill_stat(d->inode, st);
    return 0;
}

static uint64_t sys_fstat(uint64_t fd, uint64_t statbuf, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    struct kstat *st = (struct kstat *)(uintptr_t)statbuf;
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    if (!st) return -EFAULT;
    if (!t->fdt->files[fd]->inode) return -EINVAL;
    fill_stat(t->fdt->files[fd]->inode, st);
    return 0;
}

/* --------------------------------------------------------------------------
 * Phase 0: additional POSIX syscalls needed by a ported libc (musl), the
 * dynamic linker, and real coreutils.
 * -------------------------------------------------------------------------- */
#define AT_FDCWD        (-100)
#define AT_EMPTY_PATH   0x1000
#define SIG_BLOCK       0
#define SIG_UNBLOCK     1
#define SIG_SETMASK     2
#define S_IFMT_MASK     0xF000u

struct iovec { void *iov_base; size_t iov_len; };

struct rlimit64 { uint64_t rlim_cur; uint64_t rlim_max; };
#define RLIM_INFINITY64  (~0ULL)
#define RLIMIT_STACK     3
#define RLIMIT_NOFILE    7

static uint64_t sys_readv(uint64_t fd, uint64_t iovp, uint64_t cnt,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    const struct iovec *iov = (const struct iovec *)(uintptr_t)iovp;
    if (!iov || (int)cnt < 0) return -EINVAL;
    ssize_t total = 0;
    for (int i = 0; i < (int)cnt; i++) {
        if (iov[i].iov_len == 0) continue;
        ssize_t n = vfs_read(t->fdt->files[fd], iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return (total > 0) ? (uint64_t)total : (uint64_t)-EIO;
        total += n;
        if ((size_t)n < iov[i].iov_len) break;   /* short read: stop */
    }
    return (uint64_t)total;
}

static uint64_t sys_writev(uint64_t fd, uint64_t iovp, uint64_t cnt,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    const struct iovec *iov = (const struct iovec *)(uintptr_t)iovp;
    if (!iov || (int)cnt < 0) return -EINVAL;
    ssize_t total = 0;
    for (int i = 0; i < (int)cnt; i++) {
        if (iov[i].iov_len == 0) continue;
        ssize_t n = vfs_write(t->fdt->files[fd], iov[i].iov_base, iov[i].iov_len);
        if (n < 0) return (total > 0) ? (uint64_t)total : (uint64_t)-EIO;
        total += n;
        if ((size_t)n < iov[i].iov_len) break;
    }
    return (uint64_t)total;
}

static uint64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count,
                            uint64_t offset, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    struct vfs_file *f = t->fdt->files[fd];
    off_t saved = vfs_lseek(f, 0, SEEK_CUR);
    vfs_lseek(f, (off_t)offset, SEEK_SET);
    ssize_t n = vfs_read(f, (void *)(uintptr_t)buf, count);
    vfs_lseek(f, saved, SEEK_SET);
    return (n < 0) ? (uint64_t)-EIO : (uint64_t)n;
}

static uint64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t count,
                             uint64_t offset, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    struct vfs_file *f = t->fdt->files[fd];
    off_t saved = vfs_lseek(f, 0, SEEK_CUR);
    vfs_lseek(f, (off_t)offset, SEEK_SET);
    ssize_t n = vfs_write(f, (const void *)(uintptr_t)buf, count);
    vfs_lseek(f, saved, SEEK_SET);
    return (n < 0) ? (uint64_t)-EIO : (uint64_t)n;
}

/* open(2) is open as openat(AT_FDCWD, ...) in modern libc; we forward AT_FDCWD
 * and absolute paths to the regular open path. */
static uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode,
                         uint64_t a4, uint64_t a5, uint64_t a6);

static uint64_t sys_openat(uint64_t dirfd, uint64_t path, uint64_t flags,
                           uint64_t mode, uint64_t a5, uint64_t a6) {
    const char *p = (const char *)(uintptr_t)path;
    if (!p) return -EFAULT;
    if (p[0] == '/' || (int)dirfd == AT_FDCWD)
        return sys_open(path, flags, mode, a5, a6, 0);
    return -ENOSYS;   /* dirfd-relative (non-cwd) not supported yet */
}

static uint64_t sys_stat(uint64_t path, uint64_t statbuf, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6);
static uint64_t sys_fstat(uint64_t fd, uint64_t statbuf, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6);

static uint64_t sys_newfstatat(uint64_t dirfd, uint64_t path, uint64_t statbuf,
                               uint64_t flags, uint64_t a5, uint64_t a6) {
    const char *p = (const char *)(uintptr_t)path;
    if ((flags & AT_EMPTY_PATH) && (!p || !p[0]))
        return sys_fstat(dirfd, statbuf, 0, a5, a6, 0);
    if (!p) return -EFAULT;
    if (p[0] == '/' || (int)dirfd == AT_FDCWD)
        return sys_stat(path, statbuf, 0, a5, a6, 0);
    return -ENOSYS;
}

static uint64_t sys_access(uint64_t path, uint64_t mode, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
    char pbuf[1024];
    const char *p = cwd_join((const char *)(uintptr_t)path, pbuf, sizeof pbuf);
    if (!p) return -EFAULT;
    struct vfs_dentry *d = vfs_lookup_path(p);
    return (d && d->inode) ? 0 : -ENOENT;
}

static uint64_t sys_faccessat(uint64_t dirfd, uint64_t path, uint64_t mode,
                              uint64_t flags, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a5; (void)a6;
    const char *p = (const char *)(uintptr_t)path;
    if (!p) return -EFAULT;
    if (p[0] == '/' || (int)dirfd == AT_FDCWD)
        return sys_access(path, mode, 0, 0, 0, 0);
    return -ENOSYS;
}

static uint64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt) return -EBADF;
    if (oldfd == newfd) return -EINVAL;   /* dup3 differs from dup2 here */
    int r = fd_dup2(t->fdt, (int)oldfd, (int)newfd);
    return (r < 0) ? (uint64_t)-EBADF : (uint64_t)r;
}

static uint64_t sys_pipe(uint64_t fds, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6);

static uint64_t sys_pipe2(uint64_t fds, uint64_t flags, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a3; (void)a4; (void)a5; (void)a6;
    /* O_CLOEXEC/O_NONBLOCK accepted but not enforced. */
    return sys_pipe(fds, 0, 0, 0, 0, 0);
}

static uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset,
                                   uint64_t sigsetsize, uint64_t a5, uint64_t a6) {
    (void)sigsetsize; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t) return -EINVAL;
    uint64_t old = t->sig_mask;
    if (set) {
        uint64_t newset = *(const uint64_t *)(uintptr_t)set;
        switch (how) {
        case SIG_BLOCK:   t->sig_mask |= newset; break;
        case SIG_UNBLOCK: t->sig_mask &= ~newset; break;
        case SIG_SETMASK: t->sig_mask = newset; break;
        default: return -EINVAL;
        }
        /* SIGKILL/SIGSTOP cannot be blocked. */
        t->sig_mask &= ~((1ull << 9) | (1ull << 19));
    }
    if (oldset) *(uint64_t *)(uintptr_t)oldset = old;
    return 0;
}

static uint64_t sys_getrandom(uint64_t buf, uint64_t len, uint64_t flags,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)flags; (void)a4; (void)a5; (void)a6;
    uint8_t *p = (uint8_t *)(uintptr_t)buf;
    if (!p) return -EFAULT;
    /* xorshift64 seeded from the monotonic clock and the caller's tid. Not a
     * CSPRNG - sufficient for ASLR cookies / hash seeds in ported software. */
    struct thread *t = current_thread();
    uint64_t s = clock_monotonic_ns() ^ ((uint64_t)(t ? t->tid : 1) << 32);
    if (s == 0) s = 0x9e3779b97f4a7c15ULL;
    for (uint64_t i = 0; i < len; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        p[i] = (uint8_t)(s >> 24);
    }
    return (uint64_t)len;
}

static void fill_rlimit(int resource, struct rlimit64 *r) {
    switch (resource) {
    case RLIMIT_STACK:  r->rlim_cur = 8 * 1024 * 1024; r->rlim_max = RLIM_INFINITY64; break;
    case RLIMIT_NOFILE: r->rlim_cur = FD_MAX; r->rlim_max = FD_MAX; break;
    default:            r->rlim_cur = RLIM_INFINITY64; r->rlim_max = RLIM_INFINITY64; break;
    }
}

static uint64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t newp,
                              uint64_t oldp, uint64_t a5, uint64_t a6) {
    (void)pid; (void)newp; (void)a5; (void)a6;   /* setting limits is a no-op */
    if (oldp) fill_rlimit((int)resource, (struct rlimit64 *)(uintptr_t)oldp);
    return 0;
}

static uint64_t sys_getrlimit(uint64_t resource, uint64_t rlim, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (rlim) fill_rlimit((int)resource, (struct rlimit64 *)(uintptr_t)rlim);
    return 0;
}

static uint64_t sys_setrlimit(uint64_t resource, uint64_t rlim, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)resource; (void)rlim; (void)a3; (void)a4; (void)a5; (void)a6;
    return 0;   /* accepted, not enforced */
}

static uint64_t sys_chmod(uint64_t path, uint64_t mode, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    char pbuf[1024];
    const char *p = cwd_join((const char *)(uintptr_t)path, pbuf, sizeof pbuf);
    if (!p) return -EFAULT;
    struct vfs_dentry *d = vfs_lookup_path(p);
    if (!d || !d->inode) return -ENOENT;
    d->inode->mode = (d->inode->mode & S_IFMT_MASK) | (uint32_t)(mode & 07777);
    vfs_setattr(d->inode);
    return 0;
}

static uint64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    struct vfs_inode *in = t->fdt->files[fd]->inode;
    if (!in) return -EINVAL;
    in->mode = (in->mode & S_IFMT_MASK) | (uint32_t)(mode & 07777);
    vfs_setattr(in);
    return 0;
}

static uint64_t sys_umask(uint64_t mask, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t) return 022;
    uint32_t old = t->umask;
    t->umask = (uint32_t)(mask & 0777);
    return old;
}

static uint64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    struct vfs_file *f = t->fdt->files[fd];
    if (f->inode && f->inode->f_ops && f->inode->f_ops->truncate)
        return (uint64_t)(int64_t)f->inode->f_ops->truncate(f, (off_t)length);
    if (f->inode) { f->inode->size = length; return 0; }   /* best effort */
    return -EINVAL;
}

static uint64_t sys_fsync(uint64_t fd, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    return 0;   /* writes are synchronous in the current FS drivers */
}

static uint64_t sys_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz,
                             uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char *ubuf = (char *)(uintptr_t)buf;
    if (!ubuf) return -EFAULT;
    char pbuf[1024];
    const char *p = cwd_join((const char *)(uintptr_t)path, pbuf, sizeof pbuf);
    if (!p) return -EFAULT;
    int n = vfs_readlink(p, ubuf, (size_t)bufsiz);
    if (n < 0) {
        /* Distinguish "not a symlink" from "missing" for a useful errno. */
        struct vfs_dentry *d = vfs_lookup_path(p);
        return (d && d->inode) ? -EINVAL : -ENOENT;
    }
    return (uint64_t)n;
}

static uint64_t sys_symlink(uint64_t target, uint64_t linkpath, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    const char *t = (const char *)(uintptr_t)target;
    char lbuf[1024];
    const char *l = cwd_join((const char *)(uintptr_t)linkpath, lbuf, sizeof lbuf);
    if (!t || !l) return -EFAULT;
    return vfs_symlink(t, l) == 0 ? 0 : -EIO;
}

/* rename(2) for regular files implemented as copy+unlink (bounded). Cross-FS
 * and directory renames return -EXDEV/-EISDIR. */
static uint64_t sys_rename(uint64_t oldp, uint64_t newp, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    char obuf[1024], nbuf[1024];
    const char *o = cwd_join((const char *)(uintptr_t)oldp, obuf, sizeof obuf);
    const char *n = cwd_join((const char *)(uintptr_t)newp, nbuf, sizeof nbuf);
    if (!o || !n) return -EFAULT;
    struct vfs_dentry *d = vfs_lookup_path(o);
    if (!d || !d->inode) return -ENOENT;
    if (S_ISDIR(d->inode->mode)) return -EISDIR;
    uint64_t size = d->inode->size;
    if (size > (1u << 20)) return -EFBIG;   /* cap copy-based rename at 1 MiB */

    struct vfs_file *src = vfs_open(o, O_RDONLY);
    if (!src) return -ENOENT;
    struct vfs_file *dst = vfs_open(n, O_CREAT | O_WRONLY | O_TRUNC);
    if (!dst) { vfs_close(src); return -EACCES; }

    char *tmp = (char *)kmalloc(size ? size : 1);
    if (!tmp) { vfs_close(src); vfs_close(dst); return -ENOMEM; }
    ssize_t rd = vfs_read(src, tmp, size);
    if (rd > 0) vfs_write(dst, tmp, (size_t)rd);
    if (dst->inode) dst->inode->mode = d->inode->mode;
    kfree(tmp);
    vfs_close(src);
    vfs_close(dst);
    vfs_unlink(o);
    return 0;
}

static uint64_t sys_lariat_ps(uint64_t buf, uint64_t max, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct proc_info *out = (struct proc_info *)(uintptr_t)buf;
    if (!out || (int)max <= 0) return -EINVAL;
    return (uint64_t)sched_list_procs(out, (int)max);
}

/* --------------------------------------------------------------------------
 * Phase M: IPC message ports.
 * -------------------------------------------------------------------------- */
static void copy_port_name(uint64_t uptr, char *out, size_t cap) {
    const char *p = (const char *)(uintptr_t)uptr;
    size_t i = 0;
    if (p) for (; p[i] && i < cap - 1; i++) out[i] = p[i];
    out[i] = '\0';
}

static uint64_t sys_port_create(uint64_t namep, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    char name[IPC_NAME_MAX];
    copy_port_name(namep, name, sizeof(name));
    return (uint64_t)(int64_t)ipc_port_register(name, t ? t->tid : 0);
}

static uint64_t sys_port_open(uint64_t namep, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char name[IPC_NAME_MAX];
    copy_port_name(namep, name, sizeof(name));
    return (uint64_t)(int64_t)ipc_port_lookup(name);
}

static uint64_t sys_port_send(uint64_t id, uint64_t buf, uint64_t len,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    return (uint64_t)ipc_port_send((int)id, (const void *)(uintptr_t)buf,
                                   (size_t)len, t ? t->tid : 0);
}

static uint64_t sys_port_recv(uint64_t id, uint64_t buf, uint64_t max,
                              uint64_t nonblock, uint64_t a5, uint64_t a6) {
    (void)a5; (void)a6;
    return (uint64_t)ipc_port_recv((int)id, (void *)(uintptr_t)buf,
                                   (size_t)max, (int)nonblock);
}

static uint64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    struct vfs_file *f = t->fdt->files[fd];
    struct vfs_inode *inode = f->inode;
    if (!inode || !inode->i_ops || !inode->i_ops->readdir) return -ENOTDIR;

    uint8_t *out = (uint8_t *)(uintptr_t)dirp;
    size_t off = 0;
    struct vfs_dir_entry e;
    while (inode->i_ops->readdir(inode, (int)f->pos, &e) > 0) {
        size_t namelen = strlen(e.name);
        size_t reclen = (sizeof(struct dirent64) + namelen + 1 + 7) & ~(size_t)7;
        if (off + reclen > count) break;
        struct dirent64 *d = (struct dirent64 *)(out + off);
        d->d_ino = e.inode_no;
        d->d_off = (int64_t)(f->pos + 1);
        d->d_reclen = (uint16_t)reclen;
        d->d_type = (e.mode & S_IFDIR) ? DT_DIR : DT_REG;
        memcpy(d->d_name, e.name, namelen);
        d->d_name[namelen] = '\0';
        off += reclen;
        f->pos++;
    }
    return (uint64_t)off;
}

static uint64_t sys_mkdir(uint64_t path, uint64_t mode, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    char pbuf[1024];
    const char *p = cwd_join((const char *)(uintptr_t)path, pbuf, sizeof pbuf);
    if (!p) return -ENOENT;
    int r = vfs_mkdir(p, (uint32_t)mode);
    if (r == 0) {
        /* A new directory is owned by the creating user (so an unprivileged
         * user can populate dirs they make, e.g. ~/.local/bin). */
        struct thread *t = current_thread();
        struct vfs_dentry *d = vfs_lookup_path(p);
        if (t && d && d->inode) {
            d->inode->uid = t->euid;
            d->inode->gid = t->egid;
            vfs_setattr(d->inode);
        }
        return 0;
    }
    /* ramfs/ext4 return -EEXIST when the name is taken; a generic -1 means the
     * parent path could not be resolved. */
    return (uint64_t)(r == -EEXIST ? -EEXIST : -ENOENT);
}

static uint64_t sys_rmdir(uint64_t path, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char pbuf[1024];
    const char *p = cwd_join((const char *)(uintptr_t)path, pbuf, sizeof pbuf);
    if (!p) return -ENOENT;
    int r = vfs_rmdir(p);
    return (r < 0) ? (uint64_t)-ENOENT : 0;
}

static uint64_t sys_unlink(uint64_t path, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    char pbuf[1024];
    const char *p = cwd_join((const char *)(uintptr_t)path, pbuf, sizeof pbuf);
    if (!p) return -ENOENT;
    int r = vfs_unlink(p);
    return (r < 0) ? (uint64_t)-ENOENT : 0;
}

static uint64_t sys_pipe(uint64_t fds, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    int *userfds = (int *)(uintptr_t)fds;
    if (!t || !t->fdt || !userfds) return -EFAULT;

    struct vfs_file *files[2];
    int r = pipe_create(files);
    if (r < 0) return (uint64_t)r;

    int rfd = fd_alloc(t->fdt, files[0]);
    int wfd = fd_alloc(t->fdt, files[1]);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) fd_close(t->fdt, rfd); else vfs_close(files[0]);
        if (wfd >= 0) fd_close(t->fdt, wfd); else vfs_close(files[1]);
        return -EMFILE;
    }
    userfds[0] = rfd;
    userfds[1] = wfd;
    return 0;
}

/* --------------------------------------------------------------------------
 * Working directory + misc POSIX syscalls
 * -------------------------------------------------------------------------- */

/* Normalize an absolute path in place: collapse "//", ".", and ".." segments.
 * `path` must start with '/'.  The result always starts with '/' and has no
 * trailing slash (except the root "/"). */
static void normalize_path(char *path) {
    char out[256];
    size_t oi = 0;
    out[oi++] = '/';
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t seglen = (size_t)(p - seg);
        if (seglen == 1 && seg[0] == '.') {
            continue;
        }
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            if (oi > 1) {
                oi--;                      /* drop trailing '/' */
                while (oi > 1 && out[oi - 1] != '/') oi--;
            }
            continue;
        }
        if (oi > 1) {
            if (oi < sizeof(out) - 1) out[oi++] = '/';
        }
        for (size_t i = 0; i < seglen && oi < sizeof(out) - 1; i++)
            out[oi++] = seg[i];
    }
    out[oi] = '\0';
    memcpy(path, out, oi + 1);
}

static uint64_t sys_chdir(uint64_t path, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    const char *p = (const char *)(uintptr_t)path;
    if (!t || !p) return -EFAULT;

    char buf[256];
    if (p[0] == '/') {
        size_t n = strlen(p);
        if (n >= sizeof(buf)) return -ENAMETOOLONG;
        memcpy(buf, p, n + 1);
    } else {
        size_t cl = strlen(t->cwd);
        size_t pl = strlen(p);
        if (cl + 1 + pl + 1 >= sizeof(buf)) return -ENAMETOOLONG;
        memcpy(buf, t->cwd, cl);
        buf[cl] = '/';
        memcpy(buf + cl + 1, p, pl + 1);
    }
    normalize_path(buf);

    struct vfs_dentry *d = vfs_lookup_path(buf);
    if (!d || !d->inode) return -ENOENT;
    if (!S_ISDIR(d->inode->mode)) return -ENOTDIR;

    size_t n = strlen(buf);
    memcpy(t->cwd, buf, n + 1);
    return 0;
}

/* --------------------------------------------------------------------------
 * BSD socket syscalls (Linux x86_64 numbers)
 * -------------------------------------------------------------------------- */
static struct socket *sock_lookup(struct thread *t, uint64_t fd) {
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return NULL;
    return socket_from_file(t->fdt->files[fd]);
}

static uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t proto,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt) return -EINVAL;
    int err = 0;
    struct vfs_file *f = socket_create((int)domain, (int)type, (int)proto, &err);
    if (!f) return (uint64_t)(int64_t)err;
    int fd = fd_alloc(t->fdt, f);
    if (fd < 0) { vfs_close(f); return -EMFILE; }
    return (uint64_t)fd;
}

static uint64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct socket *s = sock_lookup(current_thread(), fd);
    if (!s) return -ENOTSOCK;
    return (uint64_t)(int64_t)socket_bind(s, (const struct sockaddr *)(uintptr_t)addr,
                                          (socklen_t)len);
}

static uint64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct socket *s = sock_lookup(current_thread(), fd);
    if (!s) return -ENOTSOCK;
    return (uint64_t)(int64_t)socket_connect(s, (const struct sockaddr *)(uintptr_t)addr,
                                             (socklen_t)len);
}

static uint64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct socket *s = sock_lookup(current_thread(), fd);
    if (!s) return -ENOTSOCK;
    return (uint64_t)(int64_t)socket_listen(s, (int)backlog);
}

static uint64_t sys_accept(uint64_t fd, uint64_t addr, uint64_t len,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    struct socket *s = sock_lookup(t, fd);
    if (!s) return -ENOTSOCK;
    socklen_t klen = 0, *plen = (socklen_t *)(uintptr_t)len;
    if (plen) klen = *plen;
    struct sockaddr *paddr = (struct sockaddr *)(uintptr_t)addr;
    struct vfs_file *nf = NULL;
    socklen_t outlen = klen;
    int r = socket_accept(s, paddr ? paddr : NULL, paddr ? &outlen : NULL, &nf);
    if (r < 0) return (uint64_t)(int64_t)r;
    if (plen) *plen = outlen;
    int nfd = fd_alloc(t->fdt, nf);
    if (nfd < 0) { vfs_close(nf); return -EMFILE; }
    return (uint64_t)nfd;
}

static uint64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                           uint64_t addr, uint64_t alen) {
    struct socket *s = sock_lookup(current_thread(), fd);
    if (!s) return -ENOTSOCK;
    return (uint64_t)socket_sendto(s, (const void *)(uintptr_t)buf, (size_t)len,
                                   (int)flags, (const struct sockaddr *)(uintptr_t)addr,
                                   (socklen_t)alen);
}

static uint64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                             uint64_t addr, uint64_t alen) {
    struct socket *s = sock_lookup(current_thread(), fd);
    if (!s) return -ENOTSOCK;
    socklen_t *plen = (socklen_t *)(uintptr_t)alen;
    socklen_t outlen = plen ? *plen : 0;
    struct sockaddr *paddr = (struct sockaddr *)(uintptr_t)addr;
    long r = socket_recvfrom(s, (void *)(uintptr_t)buf, (size_t)len, (int)flags,
                             paddr, paddr ? &outlen : NULL);
    if (plen && paddr) *plen = outlen;
    return (uint64_t)r;
}

static uint64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t len,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct socket *s = sock_lookup(current_thread(), fd);
    if (!s) return -ENOTSOCK;
    socklen_t *plen = (socklen_t *)(uintptr_t)len;
    socklen_t outlen = plen ? *plen : 0;
    int r = socket_getsockname(s, (struct sockaddr *)(uintptr_t)addr, &outlen);
    if (plen) *plen = outlen;
    return (uint64_t)(int64_t)r;
}

static uint64_t sys_setsockopt(uint64_t fd, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct socket *s = sock_lookup(current_thread(), fd);
    if (!s) return -ENOTSOCK;
    return 0;   /* options are accepted but ignored */
}

static uint64_t sys_getsockopt(uint64_t fd, uint64_t a2, uint64_t a3,
                               uint64_t optval, uint64_t optlen, uint64_t a6) {
    (void)a2; (void)a3; (void)a6;
    struct socket *s = sock_lookup(current_thread(), fd);
    if (!s) return -ENOTSOCK;
    socklen_t *plen = (socklen_t *)(uintptr_t)optlen;
    int *pv = (int *)(uintptr_t)optval;
    if (pv && plen && *plen >= sizeof(int)) { *pv = 0; *plen = sizeof(int); }
    return 0;
}

/* --------------------------------------------------------------------------
 * ioctl + process groups / sessions
 * -------------------------------------------------------------------------- */
static uint64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    return (uint64_t)(int64_t)vfs_ioctl(t->fdt->files[fd], (unsigned long)req,
                                        (unsigned long)arg);
}

static uint64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *self = current_thread();
    if (!self) return -ESRCH;
    struct thread *t = (pid == 0) ? self : sched_find_by_tid((uint32_t)pid);
    if (!t) return -ESRCH;
    int g = (pgid == 0) ? (int)t->tid : (int)pgid;
    t->pgid = g;
    return 0;
}

static uint64_t sys_getpgid(uint64_t pid, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *self = current_thread();
    struct thread *t = (pid == 0) ? self : sched_find_by_tid((uint32_t)pid);
    if (!t) return -ESRCH;
    return (uint64_t)(int64_t)t->pgid;
}

static uint64_t sys_getpgrp(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    return t ? (uint64_t)(int64_t)t->pgid : 0;
}

static uint64_t sys_setsid(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t) return -ESRCH;
    t->sid = (int)t->tid;
    t->pgid = (int)t->tid;
    return (uint64_t)(int64_t)t->sid;
}

static uint64_t sys_getsid(uint64_t pid, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *self = current_thread();
    struct thread *t = (pid == 0) ? self : sched_find_by_tid((uint32_t)pid);
    if (!t) return -ESRCH;
    return (uint64_t)(int64_t)t->sid;
}

static uint64_t sys_getcwd(uint64_t buf, uint64_t size, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    char *dst = (char *)(uintptr_t)buf;
    if (!t || !dst) return -EFAULT;
    size_t n = strlen(t->cwd);
    if (size < n + 1) return -ERANGE;
    memcpy(dst, t->cwd, n + 1);
    return (uint64_t)(n + 1);   /* Linux returns the buffer length used */
}

static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || !t->fdt || fd >= FD_MAX || !t->fdt->files[fd]) return -EBADF;
    struct vfs_file *f = t->fdt->files[fd];
    switch (cmd) {
        case F_DUPFD: {
            for (int i = (int)arg; i < FD_MAX; i++) {
                if (!t->fdt->files[i]) {
                    f->ref_count++;
                    t->fdt->files[i] = f;
                    return (uint64_t)i;
                }
            }
            return -EMFILE;
        }
        case F_GETFD: return 0;          /* no FD_CLOEXEC tracking */
        case F_SETFD: return 0;
        case F_GETFL: return (uint64_t)f->flags;
        case F_SETFL: f->flags = (uint32_t)arg; return 0;
        default:      return -EINVAL;
    }
}

static uint64_t sys_uname(uint64_t buf, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct utsname *u = (struct utsname *)(uintptr_t)buf;
    if (!u) return -EFAULT;
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "Lariat");
    strcpy(u->nodename, sys_hostname());
    strcpy(u->release, "0.2");
    strcpy(u->version, "Project Lariat SMP");
    strcpy(u->machine, "x86_64");
    return 0;
}

/* --------------------------------------------------------------------------
 * Signals (basic): handlers are recorded per-process, kill() sets a pending
 * bit, and pending signals are delivered on the syscall-return path (see
 * signal_deliver below, invoked from the assembly entry via C).
 * -------------------------------------------------------------------------- */
extern struct thread *sched_find_by_tid(uint32_t tid);

static uint64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (sig >= 32) return -EINVAL;
    int p = (int)pid;

    /* Negative / zero pid: signal a whole process group. */
    if (p <= 0) {
        struct thread *self = current_thread();
        int pgid = (p == 0) ? (self ? self->pgid : 0) : -p;
        if (sig == 0) return 0;
        int n = sched_signal_pgrp(pgid, (int)sig);
        return n > 0 ? 0 : (uint64_t)-ESRCH;
    }

    struct thread *target = sched_find_by_tid((uint32_t)p);
    if (!target) return -ESRCH;
    if (sig == 0) return 0;   /* existence check only */

    /* SIGKILL on self terminates immediately; otherwise deliver with job
     * control semantics (resume stopped jobs, wake blocked ones). */
    if (sig == SIGKILL && target == current_thread()) {
        target->exit_code = 128 + (int)sig;
        thread_exit();
    }
    sched_deliver_signal(target, (int)sig);
    return 0;
}

static uint64_t sys_rt_sigaction(uint64_t signum, uint64_t act, uint64_t oldact,
                                 uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t || signum >= 32) return -EINVAL;
    struct ksigaction *na = (struct ksigaction *)(uintptr_t)act;
    struct ksigaction *oa = (struct ksigaction *)(uintptr_t)oldact;
    if (oa) {
        oa->sa_handler = t->sig_handlers[signum];
        oa->sa_flags = 0;
        oa->sa_restorer = 0;
        oa->sa_mask = 0;
    }
    if (na) {
        t->sig_handlers[signum] = na->sa_handler;
        if (na->sa_restorer) t->sig_restorer = na->sa_restorer;
    }
    return 0;
}

/* Saved user context pushed onto the user stack when a handler is invoked and
 * restored by rt_sigreturn.  Only the control state (rip/rsp/rflags) is saved
 * and restored; general registers are caller-saved across the handler call. */
struct sigframe {
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t rax;       /* interrupted syscall's return value */
    uint64_t sig_mask;
};

static uint64_t sys_rt_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = current_thread();
    if (!t) return 0;
    /* The restorer trampoline issues sigreturn with the user rsp pointing at
     * the sigframe we pushed in signal_deliver, so restore the pre-signal
     * instruction pointer / stack / flags from it. */
    struct sigframe *fr = (struct sigframe *)(uintptr_t)t->tmp_rsp;
    t->tmp_rip = fr->rip;
    t->tmp_rflags = fr->rflags;
    t->tmp_rsp = fr->rsp;
    t->sig_mask = fr->sig_mask;
    return fr->rax;   /* restore the interrupted code's rax (e.g. -EINTR) */
}

/* --------------------------------------------------------------------------
 * execve (delegates to the ELF loader; falls back to the flat /init binary)
 * -------------------------------------------------------------------------- */
static uint64_t sys_execve(uint64_t pathname, uint64_t argv, uint64_t envp,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4; (void)a5; (void)a6;
    char pbuf[1024];
    const char *path = cwd_join((const char *)(uintptr_t)pathname, pbuf, sizeof pbuf);
    (void)argv; (void)envp;

    struct thread *t = current_thread();
    if (!t || !t->cr3 || !path) return -EINVAL;

    extern int elf_execve(struct thread *t, const char *path,
                          char *const argv[], char *const envp[]);
    return (uint64_t)elf_execve(t, path, (char *const *)(uintptr_t)argv,
                                (char *const *)(uintptr_t)envp);
}

/* --------------------------------------------------------------------------
 * M10: credential syscalls (uid/gid).  uid 0 is root and may set any id; a
 * non-root process may only switch among its real/effective/saved ids.
 * -------------------------------------------------------------------------- */
static uint64_t sys_getuid(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    struct thread *t = current_thread();
    return t ? t->uid : 0;
}
static uint64_t sys_geteuid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    struct thread *t = current_thread();
    return t ? t->euid : 0;
}
static uint64_t sys_getgid(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    struct thread *t = current_thread();
    return t ? t->gid : 0;
}
static uint64_t sys_getegid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    struct thread *t = current_thread();
    return t ? t->egid : 0;
}

static uint64_t sys_setuid(uint64_t uid, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    struct thread *t = current_thread();
    if (!t) return -EINVAL;
    uint32_t u = (uint32_t)uid;
    if (t->euid == 0) {                /* root: set all three */
        t->uid = t->euid = t->suid = u;
        return 0;
    }
    if (u == t->uid || u == t->suid) { /* unprivileged: effective only */
        t->euid = u;
        return 0;
    }
    return -EPERM;
}

static uint64_t sys_setgid(uint64_t gid, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    struct thread *t = current_thread();
    if (!t) return -EINVAL;
    uint32_t g = (uint32_t)gid;
    if (t->euid == 0) { t->gid = t->egid = t->sgid = g; return 0; }
    if (g == t->gid || g == t->sgid) { t->egid = g; return 0; }
    return -EPERM;
}

static uint64_t sys_setresuid(uint64_t r, uint64_t e, uint64_t s,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;(void)a5;(void)a6;
    struct thread *t = current_thread();
    if (!t) return -EINVAL;
    int32_t rr = (int32_t)r, ee = (int32_t)e, ss = (int32_t)s;
    if (t->euid != 0) {
        /* Each new id must equal one of the current real/eff/saved ids. */
        uint32_t cur[3] = { t->uid, t->euid, t->suid };
        int ok_r = (rr == -1), ok_e = (ee == -1), ok_s = (ss == -1);
        for (int i = 0; i < 3; i++) {
            if (!ok_r && (uint32_t)rr == cur[i]) ok_r = 1;
            if (!ok_e && (uint32_t)ee == cur[i]) ok_e = 1;
            if (!ok_s && (uint32_t)ss == cur[i]) ok_s = 1;
        }
        if (!ok_r || !ok_e || !ok_s) return -EPERM;
    }
    if (rr != -1) t->uid  = (uint32_t)rr;
    if (ee != -1) t->euid = (uint32_t)ee;
    if (ss != -1) t->suid = (uint32_t)ss;
    return 0;
}

static uint64_t sys_setresgid(uint64_t r, uint64_t e, uint64_t s,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;(void)a5;(void)a6;
    struct thread *t = current_thread();
    if (!t) return -EINVAL;
    int32_t rr = (int32_t)r, ee = (int32_t)e, ss = (int32_t)s;
    if (t->euid != 0) {
        uint32_t cur[3] = { t->gid, t->egid, t->sgid };
        int ok_r = (rr == -1), ok_e = (ee == -1), ok_s = (ss == -1);
        for (int i = 0; i < 3; i++) {
            if (!ok_r && (uint32_t)rr == cur[i]) ok_r = 1;
            if (!ok_e && (uint32_t)ee == cur[i]) ok_e = 1;
            if (!ok_s && (uint32_t)ss == cur[i]) ok_s = 1;
        }
        if (!ok_r || !ok_e || !ok_s) return -EPERM;
    }
    if (rr != -1) t->gid  = (uint32_t)rr;
    if (ee != -1) t->egid = (uint32_t)ee;
    if (ss != -1) t->sgid = (uint32_t)ss;
    return 0;
}

static uint64_t sys_setgroups(uint64_t size, uint64_t list, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;(void)a4;(void)a5;(void)a6;
    struct thread *t = current_thread();
    if (!t) return -EINVAL;
    if (t->euid != 0) return -EPERM;
    int n = (int)size;
    if (n < 0 || n > NGROUPS_MAX) return -EINVAL;
    const uint32_t *src = (const uint32_t *)(uintptr_t)list;
    for (int i = 0; i < n; i++) t->groups[i] = src ? src[i] : 0;
    t->ngroups = n;
    return 0;
}

static uint64_t sys_getgroups(uint64_t size, uint64_t list, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a3;(void)a4;(void)a5;(void)a6;
    struct thread *t = current_thread();
    if (!t) return -EINVAL;
    int n = (int)size;
    if (n == 0) return t->ngroups;
    if (n < t->ngroups) return -EINVAL;
    uint32_t *dst = (uint32_t *)(uintptr_t)list;
    if (dst) for (int i = 0; i < t->ngroups; i++) dst[i] = t->groups[i];
    return t->ngroups;
}

/* --------------------------------------------------------------------------
 * Signal delivery on the syscall-return path.
 *
 * For a pending signal with no handler (SIG_DFL) we perform the default action
 * (terminate).  For a handler, we redirect the user's return RIP to the
 * handler, pass the signal number in RDI, and push the original return address
 * so the handler returns straight back to the interrupted code.
 * -------------------------------------------------------------------------- */
void signal_deliver(struct thread *t, uint64_t ret_val) {
    if (!t || t->cr3 == 0 || t->sig_pending == 0) return;

    for (int s = 1; s < 32; s++) {
        uint64_t bit = (1ULL << s);
        if (!(t->sig_pending & bit)) continue;
        if (t->sig_mask & bit) continue;   /* currently blocked */
        t->sig_pending &= ~bit;

        uint64_t h = t->sig_handlers[s];
        if (h == SIG_IGN) {
            continue;
        }
        if (h == SIG_DFL) {
            /* Default actions: SIGCHLD and SIGCONT are ignored (resume is
             * handled by the sender); SIGTSTP/SIGSTOP stop the process;
             * everything else terminates. */
            if (s == SIGCHLD || s == SIGCONT) continue;
            if (s == SIGTSTP || s == SIGSTOP) {
                t->stopped = 1;
                t->stop_reported = 0;
                t->stop_sig = s;
                sched_stop_current();   /* blocks until SIGCONT re-readies us */
                t->stopped = 0;
                continue;
            }
            t->exit_code = 128 + s;
            thread_exit();   /* does not return */
        }

        uint64_t usp = t->tmp_rsp;
        usp -= 128;                       /* skip the red zone */
        usp &= ~(uint64_t)0xF;

        if (t->sig_restorer) {
            /* Save the pre-signal control state on the user stack so the
             * restorer's rt_sigreturn can put rip/rsp/rflags back, then run the
             * handler on a frame whose return address is the restorer. */
            usp -= sizeof(struct sigframe);
            struct sigframe *fr = (struct sigframe *)(uintptr_t)usp;
            fr->rip = t->tmp_rip;
            fr->rflags = t->tmp_rflags;
            fr->rsp = t->tmp_rsp;
            fr->rax = ret_val;
            fr->sig_mask = t->sig_mask;

            usp -= 8;
            *(uint64_t *)(uintptr_t)usp = t->sig_restorer;
            t->sig_mask |= bit;           /* block this signal in its handler */
        } else {
            /* No restorer: push the interrupted RIP as the return address. */
            usp -= 8;
            *(uint64_t *)(uintptr_t)usp = t->tmp_rip;
        }
        t->tmp_rsp = usp;
        t->usr_rdi = (uint64_t)s;
        t->tmp_rip = h;
        return;  /* deliver one signal per return */
    }
}

/* --------------------------------------------------------------------------
 * C handler called from assembly entry
 * -------------------------------------------------------------------------- */
uint64_t syscall_handler(uint64_t nr, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5,
                         uint64_t arg6) {
    uint64_t ret;
    if (nr >= MAX_SYSCALLS || !syscall_table[nr]) {
        ret = (uint64_t)-ENOSYS;
    } else {
        ret = syscall_table[nr](arg1, arg2, arg3, arg4, arg5, arg6);
    }
    signal_deliver(current_thread(), ret);
    return ret;
}

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */
void syscall_init(void) {
    for (int i = 0; i < MAX_SYSCALLS; i++) syscall_table[i] = NULL;

    syscall_table[SYS_READ]         = sys_read;
    syscall_table[SYS_WRITE]        = sys_write;
    syscall_table[SYS_OPEN]         = sys_open;
    syscall_table[SYS_CLOSE]        = sys_close;
    syscall_table[SYS_STAT]         = sys_stat;
    syscall_table[SYS_FSTAT]        = sys_fstat;
    syscall_table[SYS_LSEEK]        = sys_lseek;
    syscall_table[SYS_MMAP]         = sys_mmap;
    syscall_table[SYS_BRK]          = sys_brk;
    syscall_table[SYS_RT_SIGACTION] = sys_rt_sigaction;
    syscall_table[SYS_RT_SIGRETURN] = sys_rt_sigreturn;
    syscall_table[SYS_PIPE]         = sys_pipe;
    syscall_table[SYS_SCHED_YIELD]  = sys_yield;
    syscall_table[SYS_DUP]          = sys_dup;
    syscall_table[SYS_DUP2]         = sys_dup2;
    syscall_table[SYS_NANOSLEEP]    = sys_nanosleep;
    syscall_table[SYS_GETPID]       = sys_getpid;
    syscall_table[SYS_FORK]         = sys_fork;
    syscall_table[SYS_EXECVE]       = sys_execve;
    syscall_table[SYS_EXIT]         = sys_exit;
    syscall_table[SYS_EXIT_GROUP]   = sys_exit;
    syscall_table[SYS_CLONE]        = sys_clone;
    syscall_table[SYS_FUTEX]        = sys_futex;
    syscall_table[SYS_ARCH_PRCTL]   = sys_arch_prctl;
    syscall_table[SYS_SET_TID_ADDRESS] = sys_set_tid_address;
    syscall_table[SYS_GETTID]       = sys_gettid;
    syscall_table[SYS_GETUID]       = sys_getuid;
    syscall_table[SYS_GETEUID]      = sys_geteuid;
    syscall_table[SYS_GETGID]       = sys_getgid;
    syscall_table[SYS_GETEGID]      = sys_getegid;
    syscall_table[SYS_SETUID]       = sys_setuid;
    syscall_table[SYS_SETGID]       = sys_setgid;
    syscall_table[SYS_SETRESUID]    = sys_setresuid;
    syscall_table[SYS_SETRESGID]    = sys_setresgid;
    syscall_table[SYS_SETGROUPS]    = sys_setgroups;
    syscall_table[SYS_GETGROUPS]    = sys_getgroups;
    syscall_table[SYS_WAIT4]        = sys_waitpid;
    syscall_table[SYS_KILL]         = sys_kill;
    syscall_table[SYS_MKDIR]        = sys_mkdir;
    syscall_table[SYS_RMDIR]        = sys_rmdir;
    syscall_table[SYS_UNLINK]       = sys_unlink;
    syscall_table[SYS_GETPPID]      = sys_getppid;
    syscall_table[SYS_GETDENTS64]   = sys_getdents64;
    syscall_table[SYS_LARIAT_PS]    = sys_lariat_ps;
    syscall_table[SYS_UNAME]        = sys_uname;
    syscall_table[SYS_FCNTL]        = sys_fcntl;
    syscall_table[SYS_GETCWD]       = sys_getcwd;
    syscall_table[SYS_CHDIR]        = sys_chdir;
    syscall_table[SYS_SOCKET]       = sys_socket;
    syscall_table[SYS_CONNECT]      = sys_connect;
    syscall_table[SYS_ACCEPT]       = sys_accept;
    syscall_table[SYS_SENDTO]       = sys_sendto;
    syscall_table[SYS_RECVFROM]     = sys_recvfrom;
    syscall_table[SYS_BIND]         = sys_bind;
    syscall_table[SYS_LISTEN]       = sys_listen;
    syscall_table[SYS_GETSOCKNAME]  = sys_getsockname;
    syscall_table[SYS_SETSOCKOPT]   = sys_setsockopt;
    syscall_table[SYS_GETSOCKOPT]   = sys_getsockopt;
    syscall_table[SYS_CLOCK_GETTIME]= sys_clock_gettime;
    syscall_table[SYS_GETTIMEOFDAY] = sys_gettimeofday;
    syscall_table[SYS_POLL]         = sys_poll;
    syscall_table[SYS_SELECT]       = sys_select;
    syscall_table[SYS_IOCTL]        = sys_ioctl;
    syscall_table[SYS_SETPGID]      = sys_setpgid;
    syscall_table[SYS_GETPGID]      = sys_getpgid;
    syscall_table[SYS_GETPGRP]      = sys_getpgrp;
    syscall_table[SYS_SETSID]       = sys_setsid;
    syscall_table[SYS_GETSID]       = sys_getsid;

    /* Phase 0: VM, vectored/positional I/O, *at family, signals, limits. */
    syscall_table[SYS_MUNMAP]       = sys_munmap;
    syscall_table[SYS_MPROTECT]     = sys_mprotect;
    syscall_table[SYS_READV]        = sys_readv;
    syscall_table[SYS_WRITEV]       = sys_writev;
    syscall_table[SYS_PREAD64]      = sys_pread64;
    syscall_table[SYS_PWRITE64]     = sys_pwrite64;
    syscall_table[SYS_OPENAT]       = sys_openat;
    syscall_table[SYS_NEWFSTATAT]   = sys_newfstatat;
    syscall_table[SYS_ACCESS]       = sys_access;
    syscall_table[SYS_FACCESSAT]    = sys_faccessat;
    syscall_table[SYS_DUP3]         = sys_dup3;
    syscall_table[SYS_PIPE2]        = sys_pipe2;
    syscall_table[SYS_RT_SIGPROCMASK] = sys_rt_sigprocmask;
    syscall_table[SYS_GETRANDOM]    = sys_getrandom;
    syscall_table[SYS_PRLIMIT64]    = sys_prlimit64;
    syscall_table[SYS_GETRLIMIT]    = sys_getrlimit;
    syscall_table[SYS_SETRLIMIT]    = sys_setrlimit;
    syscall_table[SYS_CHMOD]        = sys_chmod;
    syscall_table[SYS_FCHMOD]       = sys_fchmod;
    syscall_table[SYS_UMASK]        = sys_umask;
    syscall_table[SYS_FTRUNCATE]    = sys_ftruncate;
    syscall_table[SYS_FSYNC]        = sys_fsync;
    syscall_table[SYS_FDATASYNC]    = sys_fsync;
    syscall_table[SYS_RENAME]       = sys_rename;
    syscall_table[SYS_READLINK]     = sys_readlink;
    syscall_table[SYS_SYMLINK]      = sys_symlink;

    /* Phase M: IPC message ports. */
    syscall_table[SYS_LARIAT_PORT_CREATE] = sys_port_create;
    syscall_table[SYS_LARIAT_PORT_OPEN]   = sys_port_open;
    syscall_table[SYS_LARIAT_PORT_SEND]   = sys_port_send;
    syscall_table[SYS_LARIAT_PORT_RECV]   = sys_port_recv;

    /* Enable SYSCALL/SYSRET on the BSP.  The control MSRs are per-CPU, so each
     * AP repeats this from ap_main via syscall_init_cpu(). */
    syscall_init_cpu();

    serial_print(SERIAL_COM1, "[SYSCALL] SYSCALL/SYSRET initialized\n");
}

/* Program the per-CPU SYSCALL/SYSRET MSRs.  The syscall dispatch table is
 * global and set up once by syscall_init(); this only touches the MSRs that
 * each core must configure independently before it can service SYSCALL. */
void syscall_init_cpu(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    uint64_t star = ((uint64_t)SEG_UCODE32 << 48) | ((uint64_t)SEG_KCODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF);
}
