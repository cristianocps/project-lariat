#include "sched.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "vfs.h"
#include "fd.h"
#include "kapi.h"
#include "errno.h"
#include "serial.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Minimal ELF64 loader + execve.
 *
 * Supports:
 *   - the embedded flat /init binary (no ELF header), and
 *   - ELF64 ET_EXEC / ET_DYN images with PT_LOAD segments.
 * Sets up a System V style initial stack (argc, argv[], envp[], NULL auxv).
 * -------------------------------------------------------------------------- */

#define ELF_MAGIC 0x464C457FU  /* 0x7F 'E' 'L' 'F' */
#define PT_LOAD   1
#define ET_EXEC   2
#define ET_DYN    3

typedef struct {
    uint32_t e_magic;       /* e_ident[0..3] */
    uint8_t  e_class;
    uint8_t  e_data;
    uint8_t  e_version0;
    uint8_t  e_osabi;
    uint8_t  e_pad[8];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

extern uint8_t _userspace_init_start[];
extern uint8_t _userspace_init_end[];
extern uint8_t _prog_sh_start[];
extern uint8_t _prog_sh_end[];
extern uint8_t _prog_ls_start[];
extern uint8_t _prog_ls_end[];
extern uint8_t _prog_cat_start[];
extern uint8_t _prog_cat_end[];
extern uint8_t _prog_echo_start[];
extern uint8_t _prog_echo_end[];
extern uint8_t _prog_hello_start[];
extern uint8_t _prog_hello_end[];
extern uint8_t _prog_httpget_start[];
extern uint8_t _prog_httpget_end[];
extern uint8_t _prog_echosrv_start[];
extern uint8_t _prog_echosrv_end[];
extern uint8_t _prog_echocli_start[];
extern uint8_t _prog_echocli_end[];

#define DECL_PROG(n) extern uint8_t _prog_##n##_start[]; extern uint8_t _prog_##n##_end[];
DECL_PROG(true) DECL_PROG(false) DECL_PROG(clear) DECL_PROG(sleep)
DECL_PROG(mkdir) DECL_PROG(rmdir) DECL_PROG(rm) DECL_PROG(cp)
DECL_PROG(mv) DECL_PROG(wc) DECL_PROG(grep) DECL_PROG(head)
DECL_PROG(tail) DECL_PROG(ps) DECL_PROG(kill)
DECL_PROG(id) DECL_PROG(whoami) DECL_PROG(login) DECL_PROG(gui)

/* Programs baked into the kernel image.  execve() resolves these paths to the
 * embedded blobs before falling back to the on-disk filesystem, so the system
 * has a usable userland even with an empty disk. */
struct embedded_prog {
    const char *path;
    uint8_t    *start;
    uint8_t    *end;
};

static const struct embedded_prog g_embedded[] = {
    { "/init",      _userspace_init_start, _userspace_init_end },
    { "/bin/sh",    _prog_sh_start,    _prog_sh_end },
    { "/bin/ls",    _prog_ls_start,    _prog_ls_end },
    { "/bin/cat",   _prog_cat_start,   _prog_cat_end },
    { "/bin/echo",  _prog_echo_start,  _prog_echo_end },
    { "/bin/hello", _prog_hello_start, _prog_hello_end },
    { "/bin/httpget", _prog_httpget_start, _prog_httpget_end },
    { "/bin/echosrv", _prog_echosrv_start, _prog_echosrv_end },
    { "/bin/echocli", _prog_echocli_start, _prog_echocli_end },
#define PROG_ENT(n) { "/bin/" #n, _prog_##n##_start, _prog_##n##_end }
    PROG_ENT(true), PROG_ENT(false), PROG_ENT(clear), PROG_ENT(sleep),
    PROG_ENT(mkdir), PROG_ENT(rmdir), PROG_ENT(rm), PROG_ENT(cp),
    PROG_ENT(mv), PROG_ENT(wc), PROG_ENT(grep), PROG_ENT(head),
    PROG_ENT(tail), PROG_ENT(ps), PROG_ENT(kill),
    PROG_ENT(id), PROG_ENT(whoami), PROG_ENT(login), PROG_ENT(gui),
    /* Note: /bin/su and /bin/passwd are intentionally NOT embedded here.  They
     * are written to the ramfs at boot as setuid-root files so that the
     * set-user-ID-on-exec path (which only fires for VFS-backed binaries) can
     * elevate an unprivileged caller. */
};

static const struct embedded_prog *find_embedded(const char *path) {
    for (size_t i = 0; i < sizeof(g_embedded) / sizeof(g_embedded[0]); i++) {
        if (strcmp(path, g_embedded[i].path) == 0) return &g_embedded[i];
    }
    return NULL;
}

static uint64_t *cr3_ptr(struct thread *t) {
    return (uint64_t *)phys_to_virt(t->cr3);
}

/* Map (and zero) one user page, then copy `len` bytes from `src` at `off`. */
static int map_and_fill(struct thread *t, uint64_t vaddr, const uint8_t *src,
                        size_t off, size_t filesz, size_t memsz, uint64_t flags) {
    uint64_t start = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (vaddr + memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t file_end = vaddr + filesz;     /* first byte past file-backed data */
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -ENOMEM;
        uint8_t *kp = (uint8_t *)phys_to_virt(phys);
        memset(kp, 0, PAGE_SIZE);

        /* Copy this page's slice of the file image through the kernel direct
         * map (NOT the user virtual address): exec remaps these vaddrs, and a
         * stale TLB entry for the previous program would otherwise make a
         * user-vaddr copy land on the wrong physical page. */
        if (src && filesz && v < file_end) {
            uint64_t copy_start = v > vaddr ? v : vaddr;
            uint64_t copy_end   = v + PAGE_SIZE < file_end ? v + PAGE_SIZE : file_end;
            if (copy_end > copy_start) {
                memcpy(kp + (copy_start - v),
                       src + off + (copy_start - vaddr),
                       (size_t)(copy_end - copy_start));
            }
        }

        if (vmm_map_page_in(cr3_ptr(t), v, phys,
                            PT_USER | PT_PRESENT | flags) < 0) {
            pmm_free_page(phys);
            return -ENOMEM;
        }
    }
    return 0;
}

/* Build the initial user stack: strings, argv[], envp[], argc. Returns rsp. */
static uint64_t setup_user_stack(struct thread *t, uint64_t stack_top,
                                 char *const argv[], char *const envp[]) {
    int argc = 0, envc = 0;
    if (argv) while (argv[argc]) argc++;
    if (envp) while (envp[envc]) envc++;

    uint64_t sp = stack_top;
    /* Copy strings to the top of the stack. */
    uint64_t argp[32], envpp[32];
    if (argc > 31) argc = 31;
    if (envc > 31) envc = 31;

    for (int i = 0; i < envc; i++) {
        size_t l = strlen(envp[i]) + 1;
        sp -= l;
        memcpy((void *)(uintptr_t)sp, envp[i], l);
        envpp[i] = sp;
    }
    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        sp -= l;
        memcpy((void *)(uintptr_t)sp, argv[i], l);
        argp[i] = sp;
    }

    sp &= ~(uint64_t)0xF;  /* align */

    /* Layout (top-down): argc, argv[], NULL, envp[], NULL */
    uint64_t entries = 1 + (argc + 1) + (envc + 1);
    if (entries & 1) sp -= 8;  /* keep argc 16-byte aligned after pushes */

    uint64_t *stk = (uint64_t *)(uintptr_t)sp;
    stk -= entries;
    uint64_t idx = 0;
    stk[idx++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) stk[idx++] = argp[i];
    stk[idx++] = 0;
    for (int i = 0; i < envc; i++) stk[idx++] = envpp[i];
    stk[idx++] = 0;

    return (uint64_t)(uintptr_t)stk;
}

static void reset_address_space(struct thread *t) {
    /* Drop the previous program's known fixed regions.  (Heap/mmap arenas are
     * abandoned; their frames are recovered when the page table is destroyed
     * at process exit.) */
    vmm_unmap_page_in(cr3_ptr(t), USER_CODE_START);
    uint64_t stack_virt = USER_STACK_TOP - USER_STACK_SIZE;
    vmm_unmap_range_in(cr3_ptr(t), stack_virt, USER_STACK_SIZE / PAGE_SIZE);
    t->brk_start = t->brk_cur = 0;
    t->mmap_next = 0;
}

int elf_execve(struct thread *t, const char *path,
               char *const argv[], char *const envp[]) {
    if (!t || !t->cr3 || !path) return -EINVAL;

    /* Acquire the program image into a kernel buffer. */
    const uint8_t *image = NULL;
    size_t image_len = 0;
    uint8_t *heap_buf = NULL;

    /* set-user/group-ID-on-exec state captured from the on-disk inode. */
    uint32_t exec_mode = 0, exec_uid = 0, exec_gid = 0;

    const struct embedded_prog *emb = find_embedded(path);
    if (emb) {
        image = emb->start;
        image_len = (size_t)(emb->end - emb->start);
    } else {
        struct vfs_file *f = vfs_open(path, O_RDONLY);
        if (!f) return -ENOENT;
        if (f->inode) {
            /* The caller must have execute permission on the file. */
            if (vfs_permission(f->inode, MAY_EXEC) < 0) {
                vfs_close(f);
                return -EACCES;
            }
            exec_mode = f->inode->mode;
            exec_uid  = f->inode->uid;
            exec_gid  = f->inode->gid;
        }
        uint32_t sz = f->inode ? f->inode->size : 0;
        if (sz == 0 || sz > (16u << 20)) { vfs_close(f); return -ENOEXEC; }
        heap_buf = kmalloc(sz);
        if (!heap_buf) { vfs_close(f); return -ENOMEM; }
        ssize_t n = vfs_read(f, heap_buf, sz);
        vfs_close(f);
        if (n <= 0) { kfree(heap_buf); return -ENOEXEC; }
        image = heap_buf;
        image_len = (size_t)n;
    }

    /* Snapshot argv/envp into kernel memory *before* tearing down the old
     * address space: the user-supplied pointers (and the strings they point to)
     * live in the calling process's now-doomed pages, so we must copy them out
     * first and build the new initial stack from the kernel-resident copies. */
#define EXEC_MAXARG 64
#define EXEC_ARGBUF 4096
    char *kargv[EXEC_MAXARG + 1];
    char *kenvp[EXEC_MAXARG + 1];
    char *argbuf = kmalloc(EXEC_ARGBUF);
    if (!argbuf) { if (heap_buf) kfree(heap_buf); return -ENOMEM; }
    size_t boff = 0;
    int kac = 0, kec = 0;
    if (argv) {
        for (; argv[kac] && kac < EXEC_MAXARG; kac++) {
            size_t l = strlen(argv[kac]) + 1;
            if (boff + l > EXEC_ARGBUF) break;
            memcpy(argbuf + boff, argv[kac], l);
            kargv[kac] = argbuf + boff;
            boff += l;
        }
    }
    kargv[kac] = NULL;
    if (envp) {
        for (; envp[kec] && kec < EXEC_MAXARG; kec++) {
            size_t l = strlen(envp[kec]) + 1;
            if (boff + l > EXEC_ARGBUF) break;
            memcpy(argbuf + boff, envp[kec], l);
            kenvp[kec] = argbuf + boff;
            boff += l;
        }
    }
    kenvp[kec] = NULL;

    /* Record the program basename for ps(1). */
    {
        const char *base = path;
        for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
        size_t i = 0;
        for (; base[i] && i < sizeof(t->name) - 1; i++) t->name[i] = base[i];
        t->name[i] = '\0';
    }

    uint64_t entry;
    reset_address_space(t);

    /* exec resets signal dispositions to the default and clears any pending or
     * blocked signals, so a new program never inherits the previous one's
     * handlers (e.g. the shell's SIGINT catcher). */
    for (int i = 0; i < 32; i++) t->sig_handlers[i] = 0;
    t->sig_pending = 0;
    t->sig_mask = 0;
    t->sig_restorer = 0;

    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
    if (image_len >= sizeof(elf64_ehdr_t) && eh->e_magic == ELF_MAGIC &&
        (eh->e_type == ET_EXEC || eh->e_type == ET_DYN)) {
        /* Proper ELF64 image. */
        entry = eh->e_entry;
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(image + eh->e_phoff);
        for (int i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type != PT_LOAD) continue;
            uint64_t flags = (ph[i].p_flags & 0x2) ? PT_WRITABLE : 0;
            if (map_and_fill(t, ph[i].p_vaddr, image, ph[i].p_offset,
                             ph[i].p_filesz, ph[i].p_memsz, flags) < 0) {
                if (heap_buf) kfree(heap_buf);
                kfree(argbuf);
                return -ENOMEM;
            }
        }
    } else {
        /* Flat binary: load verbatim at USER_CODE_START. */
        entry = USER_CODE_START;
        if (map_and_fill(t, USER_CODE_START, image, 0, image_len, image_len,
                         PT_WRITABLE) < 0) {
            if (heap_buf) kfree(heap_buf);
            kfree(argbuf);
            return -ENOMEM;
        }
    }

    if (heap_buf) kfree(heap_buf);

    /* Apply set-user/group-ID-on-exec: a successful exec of a setuid binary
     * elevates the effective (and saved) IDs to the file owner's. */
    if (exec_mode & S_ISUID) { t->euid = exec_uid; t->suid = exec_uid; }
    if (exec_mode & S_ISGID) { t->egid = exec_gid; t->sgid = exec_gid; }

    /* Fresh user stack. */
    size_t stack_pages = USER_STACK_SIZE / PAGE_SIZE;
    uint64_t stack_virt = USER_STACK_TOP - USER_STACK_SIZE;
    for (size_t i = 0; i < stack_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -ENOMEM;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        if (vmm_map_page_in(cr3_ptr(t), stack_virt + i * PAGE_SIZE, phys,
                            PT_USER | PT_PRESENT | PT_WRITABLE) < 0) {
            pmm_free_page(phys);
            kfree(argbuf);
            return -ENOMEM;
        }
    }

    uint64_t rsp = setup_user_stack(t, USER_STACK_TOP, kargv, kenvp);
    kfree(argbuf);

    /* Close cloexec-less fds >= 3 (we have no O_CLOEXEC; keep stdio). */
    for (int i = 3; i < FD_MAX; i++) {
        if (t->fdt && t->fdt->files[i]) {
            vfs_close(t->fdt->files[i]);
            t->fdt->files[i] = NULL;
        }
    }

    t->user_rsp = rsp;
    t->tmp_rip = entry;
    t->tmp_rsp = rsp;
    t->tmp_rflags = 0x202;
    return 0;
}

