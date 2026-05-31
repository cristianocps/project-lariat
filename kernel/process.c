#include "process.h"
#include "sched.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "gdt.h"
#include "serial.h"
#include "kapi.h"
#include "fd.h"
#include <string.h>

extern void enter_userspace(uint64_t rip, uint64_t rsp);
extern void thread_trampoline(void);

/* --------------------------------------------------------------------------
 * Create a user process from a flat binary
 * -------------------------------------------------------------------------- */
struct thread *process_create_flat(const uint8_t *code, size_t len) {
    /* 1. Allocate a new page table with kernel mappings cloned */
    uint64_t pml4_phys = vmm_clone_kernel_pagetable();
    if (!pml4_phys) {
        serial_print(SERIAL_COM1, "[PROC] Failed to allocate page table\n");
        return NULL;
    }
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

    /* 2. Allocate physical pages for code and copy binary */
    size_t code_pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t code_phys = pmm_alloc_pages(code_pages);
    if (!code_phys) {
        serial_print(SERIAL_COM1, "[PROC] Failed to allocate code pages\n");
        pmm_free_page(pml4_phys);
        return NULL;
    }
    /* Copy the binary into the freshly allocated physical pages via the direct
     * map.  Use inline asm to keep the compiler from eliding the copy. */
    uint64_t code_dst = (uint64_t)phys_to_virt(code_phys);
    __asm__ __volatile__ (
        "movq %0, %%rdi\n"
        "movq %1, %%rsi\n"
        "movq %2, %%rcx\n"
        "rep movsb\n"
        :: "r"(code_dst), "r"((uint64_t)code), "r"(len)
        : "rdi", "rsi", "rcx", "memory"
    );
    if (vmm_map_range_in(pml4, USER_CODE_START, code_phys, code_pages,
            PT_USER | PT_PRESENT | PT_WRITABLE) < 0) {
        serial_print(SERIAL_COM1, "[PROC] Failed to map code pages\n");
        pmm_free_pages(code_phys, code_pages);
        pmm_free_page(pml4_phys);
        return NULL;
    }

    /* 3. Allocate and map user stack */
    size_t stack_pages = USER_STACK_SIZE / PAGE_SIZE;
    uint64_t stack_phys = pmm_alloc_pages(stack_pages);
    if (!stack_phys) {
        serial_print(SERIAL_COM1, "[PROC] Failed to allocate user stack\n");
        vmm_unmap_page_in(pml4, USER_CODE_START);
        pmm_free_pages(code_phys, code_pages);
        pmm_free_page(pml4_phys);
        return NULL;
    }
    memset(phys_to_virt(stack_phys), 0, USER_STACK_SIZE);
    uint64_t stack_virt = USER_STACK_TOP - USER_STACK_SIZE;
    for (size_t i = 0; i < stack_pages; i++) {
        if (vmm_map_page_in(pml4, stack_virt + i * PAGE_SIZE,
                stack_phys + i * PAGE_SIZE,
                PT_USER | PT_PRESENT | PT_WRITABLE) < 0) {
            serial_print(SERIAL_COM1, "[PROC] Failed to map user stack\n");
            vmm_unmap_range_in(pml4, stack_virt, i);
            pmm_free_pages(stack_phys, stack_pages);
            vmm_unmap_page_in(pml4, USER_CODE_START);
            pmm_free_pages(code_phys, code_pages);
            pmm_free_page(pml4_phys);
            return NULL;
        }
    }

    /* 4. Allocate kernel stack for syscall/irq handling */
    void *kstack_pages = alloc_pages(2);
    if (!kstack_pages) {
        serial_print(SERIAL_COM1, "[PROC] Failed to allocate kernel stack\n");
        vmm_unmap_range_in(pml4, stack_virt, stack_pages);
        pmm_free_pages(stack_phys, stack_pages);
        vmm_unmap_page_in(pml4, USER_CODE_START);
        pmm_free_pages(code_phys, code_pages);
        pmm_free_page(pml4_phys);
        return NULL;
    }
    uint64_t kstack_top = (uint64_t)kstack_pages + 8192;

    /* 5. Allocate and set up thread structure */
    struct thread *t = kzalloc(sizeof(struct thread));
    if (!t) {
        free_pages(kstack_pages, 2);
        vmm_unmap_range_in(pml4, stack_virt, stack_pages);
        pmm_free_pages(stack_phys, stack_pages);
        vmm_unmap_page_in(pml4, USER_CODE_START);
        pmm_free_pages(code_phys, code_pages);
        pmm_free_page(pml4_phys);
        return NULL;
    }

    t->tid = 0;  /* Will be set by scheduler when added to queue */
    t->state = THREAD_READY;
    t->entry = NULL;
    t->arg = NULL;
    t->stack_size = 8192;
    t->stack_base = kstack_top;
    t->cr3 = pml4_phys;
    t->kernel_stack = kstack_top;
    t->user_rsp = USER_STACK_TOP;

    /* Allocate file descriptor table and wire up stdin/stdout/stderr to the
     * console device. */
    t->fdt = fd_table_alloc();
    if (t->fdt) {
        t->fdt->files[0] = console_open();
        t->fdt->files[1] = console_open();
        t->fdt->files[2] = console_open();
    }
    if (!t->fdt) {
        serial_print(SERIAL_COM1, "[PROC] Failed to allocate fd table\n");
        kfree(t);
        free_pages(kstack_pages, 2);
        vmm_unmap_range_in(pml4, stack_virt, stack_pages);
        pmm_free_pages(stack_phys, stack_pages);
        vmm_unmap_page_in(pml4, USER_CODE_START);
        pmm_free_pages(code_phys, code_pages);
        pmm_free_page(pml4_phys);
        return NULL;
    }

    /*
     * Set up initial kernel stack for switch_thread:
     *
     * switch_thread pops: r15, r14, r13, r12, rbp, rbx
     * then ret to the return address on stack.
     *
     * When this user thread is first scheduled, it will resume in
     * thread_trampoline, which detects cr3 != 0 and calls enter_userspace.
     */
    uint64_t *sp = (uint64_t *)kstack_top;
    *--sp = (uint64_t)thread_trampoline;  /* return address */
    *--sp = 0;  /* rbx */
    *--sp = 0;  /* rbp */
    *--sp = 0;  /* r12 */
    *--sp = 0;  /* r13 */
    *--sp = 0;  /* r14 */
    *--sp = 0;  /* r15 */

    t->rsp = (uint64_t)sp;

    /* 6. Add to scheduler */
    sched_enqueue_thread(t);

    serial_printf(SERIAL_COM1,
        "[PROC] User process created: tid=%d cr3=%x code@%x stack@%x kstack=%x\n",
        t->tid, (uint64_t)pml4_phys, USER_CODE_START, USER_STACK_TOP, kstack_top);

    return t;
}

/* --------------------------------------------------------------------------
 * Create a user process that loads an ELF (or flat) image by path on its first
 * dispatch.  Unlike process_create_flat (which copies a flat blob from kernel
 * context), this defers loading to thread_trampoline, which runs *inside* the
 * new address space and can therefore use the full ELF loader (mapping .bss,
 * setting up a SysV argv/envp stack, etc).
 * -------------------------------------------------------------------------- */
struct thread *process_create_user(const char *path) {
    uint64_t pml4_phys = vmm_clone_kernel_pagetable();
    if (!pml4_phys) {
        serial_print(SERIAL_COM1, "[PROC] Failed to allocate page table\n");
        return NULL;
    }

    void *kstack_pages = alloc_pages(2);
    if (!kstack_pages) {
        serial_print(SERIAL_COM1, "[PROC] Failed to allocate kernel stack\n");
        vmm_destroy_pagetable(pml4_phys);
        return NULL;
    }
    uint64_t kstack_top = (uint64_t)kstack_pages + 8192;

    struct thread *t = kzalloc(sizeof(struct thread));
    if (!t) {
        free_pages(kstack_pages, 2);
        vmm_destroy_pagetable(pml4_phys);
        return NULL;
    }

    t->tid = 0;
    t->state = THREAD_READY;
    t->stack_size = 8192;
    t->stack_base = kstack_top;
    t->cr3 = pml4_phys;
    t->kernel_stack = kstack_top;
    t->user_rsp = USER_STACK_TOP;
    t->exec_path = path;
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
    t->umask = 022;   /* conventional default file-creation mask */

    t->fdt = fd_table_alloc();
    if (!t->fdt) {
        kfree(t);
        free_pages(kstack_pages, 2);
        vmm_destroy_pagetable(pml4_phys);
        return NULL;
    }
    t->fdt->files[0] = console_open();
    t->fdt->files[1] = console_open();
    t->fdt->files[2] = console_open();

    /* First dispatch resumes in thread_trampoline (which runs elf_execve in
     * this thread's address space, then enters ring 3). */
    uint64_t *sp = (uint64_t *)kstack_top;
    *--sp = (uint64_t)thread_trampoline;
    *--sp = 0;  /* rbx */
    *--sp = 0;  /* rbp */
    *--sp = 0;  /* r12 */
    *--sp = 0;  /* r13 */
    *--sp = 0;  /* r14 */
    *--sp = 0;  /* r15 */
    t->rsp = (uint64_t)sp;

    sched_enqueue_thread(t);
    serial_printf(SERIAL_COM1,
        "[PROC] User process queued: tid=%d cr3=%x exec=%s\n",
        t->tid, (uint64_t)pml4_phys, path);
    return t;
}

/* --------------------------------------------------------------------------
 * fork_return - called by switch_thread when a forked child is first run
 * -------------------------------------------------------------------------- */
extern void fork_return_asm(void);

/* --------------------------------------------------------------------------
 * Fork the current user process
 * -------------------------------------------------------------------------- */
struct thread *process_fork(struct thread *parent) {
    /* Must be called from syscall context with interrupts disabled */
    if (!parent || parent->cr3 == 0) {
        serial_print(SERIAL_COM1, "[PROC] fork: not a user thread\n");
        return NULL;
    }

    /* 1. Clone page table */
    uint64_t pml4_phys = vmm_clone_pagetable((uint64_t *)phys_to_virt(parent->cr3));
    if (!pml4_phys) {
        serial_print(SERIAL_COM1, "[PROC] fork: failed to clone page table\n");
        return NULL;
    }

    /* 2. Allocate kernel stack */
    void *kstack_pages = alloc_pages(2);
    if (!kstack_pages) {
        serial_print(SERIAL_COM1, "[PROC] fork: failed to allocate kernel stack\n");
        pmm_free_page(pml4_phys);
        return NULL;
    }
    uint64_t kstack_top = (uint64_t)kstack_pages + 8192;

    /* 3. Create child thread structure */
    struct thread *child = kzalloc(sizeof(struct thread));
    if (!child) {
        serial_print(SERIAL_COM1, "[PROC] fork: failed to allocate thread\n");
        free_pages(kstack_pages, 2);
        pmm_free_page(pml4_phys);
        return NULL;
    }

    /* Copy fields from parent */
    memcpy(child, parent, sizeof(struct thread));
    child->tid = 0;
    child->tgid = 0;   /* fork() makes a new process: tgid == tid at enqueue */
    child->state = THREAD_READY;
    child->next = NULL;
    child->parent = parent;
    child->children = NULL;
    child->sibling = parent->children;
    parent->children = child;
    child->exit_code = 0;
    child->waited = 0;

    /* New kernel stack and page table */
    child->stack_base = kstack_top;
    child->stack_size = 8192;
    child->kernel_stack = kstack_top;
    child->cr3 = pml4_phys;

    /* Clone fd table */
    child->fdt = fd_table_clone(parent->fdt);
    if (!child->fdt) {
        kfree(child);
        free_pages(kstack_pages, 2);
        pmm_free_page(pml4_phys);
        return NULL;
    }

    /* Save syscall return state from parent */
    child->fork_rip = parent->tmp_rip;
    child->fork_rflags = parent->tmp_rflags;
    child->fork_rsp = parent->tmp_rsp;
    child->fork_rax = 0;

    /* Set up child kernel stack for switch_thread:
     * switch_thread pops: r15, r14, r13, r12, rbp, rbx
     * then ret to fork_return_asm
     */
    uint64_t *sp = (uint64_t *)kstack_top;
    *--sp = (uint64_t)fork_return_asm;
    *--sp = 0;  /* rbx */
    *--sp = 0;  /* rbp */
    *--sp = 0;  /* r12 */
    *--sp = 0;  /* r13 */
    *--sp = 0;  /* r14 */
    *--sp = 0;  /* r15 */

    child->rsp = (uint64_t)sp;

    /* Add to scheduler */
    sched_enqueue_thread(child);

    return child;
}

/* --------------------------------------------------------------------------
 * clone(2): create a thread (shared address space) or a fork-like child.
 *
 * The mechanics mirror process_fork - the child first runs fork_return_asm,
 * which iretq's to the parent's syscall-return RIP with rax==0 - but with two
 * differences driven by the clone flags:
 *   - CLONE_VM shares the parent cr3 (refcounted) instead of cloning it, so the
 *     two threads see the same memory; CLONE_FILES likewise shares the fd table.
 *   - the child resumes on a caller-provided user stack and (optionally) with a
 *     fresh TLS base, which is what a pthread needs.
 * -------------------------------------------------------------------------- */
struct thread *process_clone(struct thread *parent, uint64_t flags,
                             uint64_t new_stack, uint64_t tls,
                             int *ptid, int *ctid) {
    if (!parent || parent->cr3 == 0)
        return NULL;

    int share_vm = (flags & CLONE_VM) != 0;

    /* Address space: share (refcounted) or copy. */
    uint64_t pml4_phys;
    int *mm_count = NULL;
    if (share_vm) {
        pml4_phys = parent->cr3;
        /* Promote the parent to a refcounted address space on first share. */
        if (!parent->mm_count) {
            mm_count = kzalloc(sizeof(int));
            if (!mm_count) return NULL;
            *mm_count = 1;            /* the parent */
            parent->mm_count = mm_count;
        }
        mm_count = parent->mm_count;
        (*mm_count)++;               /* the child */
    } else {
        pml4_phys = vmm_clone_pagetable((uint64_t *)phys_to_virt(parent->cr3));
        if (!pml4_phys) return NULL;
    }

    void *kstack_pages = alloc_pages(2);
    if (!kstack_pages) {
        if (share_vm) (*mm_count)--;
        else pmm_free_page(pml4_phys);
        return NULL;
    }
    uint64_t kstack_top = (uint64_t)kstack_pages + 8192;

    struct thread *child = kzalloc(sizeof(struct thread));
    if (!child) {
        free_pages(kstack_pages, 2);
        if (share_vm) (*mm_count)--;
        else pmm_free_page(pml4_phys);
        return NULL;
    }

    memcpy(child, parent, sizeof(struct thread));
    child->tid = 0;
    /* CLONE_THREAD (share_vm pthread) stays in the parent's process: keep the
     * inherited tgid.  Otherwise this is a new process: clear tgid so the
     * scheduler assigns tgid == tid at enqueue. */
    if (!share_vm) child->tgid = 0;
    child->state = THREAD_READY;
    child->next = NULL;
    child->all_next = NULL;
    child->parent = parent;
    child->children = NULL;
    child->sibling = parent->children;
    parent->children = child;
    child->exit_code = 0;
    child->waited = 0;
    child->stopped = 0;
    child->stop_reported = 0;
    child->wait_q = NULL;

    child->stack_base = kstack_top;
    child->stack_size = 8192;
    child->kernel_stack = kstack_top;
    child->cr3 = pml4_phys;
    child->mm_count = share_vm ? mm_count : NULL;

    /* File descriptors: share (bump refcount) or copy. */
    if (flags & CLONE_FILES) {
        child->fdt = parent->fdt;
        if (child->fdt) child->fdt->ref_count++;
    } else {
        child->fdt = fd_table_clone(parent->fdt);
    }
    if (!child->fdt) {
        if (share_vm) (*mm_count)--;
        else vmm_destroy_pagetable(pml4_phys);
        free_pages(kstack_pages, 2);
        parent->children = child->sibling;
        kfree(child);
        return NULL;
    }

    /* TLS base for the new thread. */
    if (flags & CLONE_SETTLS)
        child->fs_base = tls;
    child->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? (uint64_t)(uintptr_t)ctid : 0;

    /* Resume at the parent's syscall return point, but on the new user stack and
     * with rax == 0 (the child's clone() return value). */
    child->fork_rip = parent->tmp_rip;
    child->fork_rflags = parent->tmp_rflags;
    child->fork_rsp = new_stack ? new_stack : parent->tmp_rsp;
    child->fork_rax = 0;

    uint64_t *sp = (uint64_t *)kstack_top;
    *--sp = (uint64_t)fork_return_asm;
    *--sp = 0;  /* rbx */
    *--sp = 0;  /* rbp */
    *--sp = 0;  /* r12 */
    *--sp = 0;  /* r13 */
    *--sp = 0;  /* r14 */
    *--sp = 0;  /* r15 */
    child->rsp = (uint64_t)sp;

    /* Assign the tid and publish it to ptid/ctid before the child becomes
     * runnable, so a fast-exiting child cannot clear the join word before the
     * parent has written it. */
    int *pset = (flags & CLONE_PARENT_SETTID) ? ptid : NULL;
    int *cset = (flags & CLONE_CHILD_SETTID)  ? ctid : NULL;
    sched_enqueue_thread_tid(child, pset, cset);

    return child;
}
