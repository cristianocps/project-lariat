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
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define ET_EXEC   2
#define ET_DYN    3

/* Dynamic section tags (subset needed for RELA relocations). */
#define DT_NULL     0
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_RELACOUNT 0x6ffffff9

/* x86_64 relocation type for base-relative fixups (the only kind a statically
 * linked PIE / -no-undefined shared object emits). */
#define R_X86_64_RELATIVE 8
#define ELF64_R_TYPE(i)   ((uint32_t)((i) & 0xffffffff))

/* Auxiliary vector entry types (SysV ABI). */
#define AT_NULL    0
#define AT_IGNORE  1
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_FLAGS   8
#define AT_ENTRY   9
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_HWCAP   16
#define AT_CLKTCK  17
#define AT_SECURE  23
#define AT_RANDOM  25
#define AT_EXECFN  31

typedef struct { int64_t d_tag; uint64_t d_val; } elf64_dyn_t;
typedef struct { uint64_t r_offset; uint64_t r_info; int64_t r_addend; } elf64_rela_t;

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
DECL_PROG(lpkg) DECL_PROG(useradd) DECL_PROG(userdel) DECL_PROG(settings)
DECL_PROG(lcc)

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
    PROG_ENT(lpkg), PROG_ENT(useradd), PROG_ENT(userdel), PROG_ENT(settings),
    PROG_ENT(lcc),
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

/* Materialize the kernel-embedded /bin programs as real files in the (ramfs)
 * root at boot, so /bin behaves like a normal Unix directory: ls/cp/stat see
 * real inodes, and command resolution is plain PATH-based file lookup rather
 * than a magic in-kernel table.  Called once from boot, after / and /bin exist
 * and before userspace init runs.  The g_embedded table stays as a last-resort
 * exec fallback (e.g. if this write ever fails).  Entries already present on
 * disk (setuid /bin/su, /bin/passwd, or a package-installed override) are left
 * untouched. */
void elf_install_embedded_programs(void) {
    for (size_t i = 0; i < sizeof(g_embedded) / sizeof(g_embedded[0]); i++) {
        const char *path = g_embedded[i].path;
        if (strncmp(path, "/bin/", 5) != 0) continue;   /* skip /init */

        struct vfs_file *ex = vfs_open(path, O_RDONLY);
        if (ex) { vfs_close(ex); continue; }            /* already a real file */

        struct vfs_file *f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC);
        if (!f) {
            serial_printf(SERIAL_COM1, "[BIN] failed to install %s\n", path);
            continue;
        }
        size_t len = (size_t)(g_embedded[i].end - g_embedded[i].start);
        vfs_write(f, g_embedded[i].start, len);
        if (f->inode) f->inode->mode = S_IFREG | 0755;  /* world-executable */
        vfs_close(f);
    }
    serial_print(SERIAL_COM1, "[BIN] embedded programs materialized in /bin\n");
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

/* Like map_and_fill, but stream the file-backed bytes directly from an open
 * file (positioned reads) rather than from an in-memory image.  This lets us
 * load arbitrarily large executables (e.g. gcc's multi-megabyte cc1) without
 * buffering the whole file in one contiguous kernel allocation. */
static int map_and_fill_file(struct thread *t, uint64_t vaddr,
                             struct vfs_file *file, size_t off,
                             size_t filesz, size_t memsz, uint64_t flags) {
    uint64_t start = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (vaddr + memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t file_end = vaddr + filesz;
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -ENOMEM;
        uint8_t *kp = (uint8_t *)phys_to_virt(phys);
        memset(kp, 0, PAGE_SIZE);

        if (filesz && v < file_end) {
            uint64_t copy_start = v > vaddr ? v : vaddr;
            uint64_t copy_end   = v + PAGE_SIZE < file_end ? v + PAGE_SIZE : file_end;
            if (copy_end > copy_start) {
                size_t want = (size_t)(copy_end - copy_start);
                off_t fpos = (off_t)(off + (copy_start - vaddr));
                if (vfs_lseek(file, fpos, SEEK_SET) != fpos) {
                    pmm_free_page(phys);
                    return -ENOEXEC;
                }
                uint8_t *dst = kp + (copy_start - v);
                size_t got = 0;
                while (got < want) {
                    ssize_t n = vfs_read(file, dst + got, want - got);
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                if (got != want) { pmm_free_page(phys); return -ENOEXEC; }
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

/* A single auxv key/value pair to place on the initial stack. */
struct auxval { uint64_t key; uint64_t val; };

/* Build the initial user stack: strings, auxv data, then the SysV vector
 * argc, argv[], NULL, envp[], NULL, auxv[], AT_NULL. Returns the final rsp. */
static uint64_t setup_user_stack(struct thread *t, uint64_t stack_top,
                                 char *const argv[], char *const envp[],
                                 const struct auxval *aux, int naux) {
    (void)t;
    int argc = 0, envc = 0;
    if (argv) while (argv[argc]) argc++;
    if (envp) while (envp[envc]) envc++;

    uint64_t sp = stack_top;
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

    /* 16 random bytes for AT_RANDOM (consumed by libc stack-guard / ASLR). */
    sp -= 16;
    uint64_t at_random = sp;
    {
        uint64_t r = (uint64_t)stack_top ^ ((uint64_t)(uintptr_t)argv << 7) ^ 0x9e3779b97f4a7c15ULL;
        for (int i = 0; i < 16; i++) { r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            ((uint8_t *)(uintptr_t)at_random)[i] = (uint8_t)(r >> 24); }
    }

    sp &= ~(uint64_t)0xF;  /* align */

    /* Count words: argc, argv[]+NULL, envp[]+NULL, auxv[]+(AT_NULL pair). */
    uint64_t auxwords = (uint64_t)(naux + 1) * 2;
    uint64_t entries = 1 + (argc + 1) + (envc + 1) + auxwords;
    if (entries & 1) sp -= 8;  /* keep argc 16-byte aligned after pushes */

    uint64_t *stk = (uint64_t *)(uintptr_t)sp;
    stk -= entries;
    uint64_t idx = 0;
    stk[idx++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) stk[idx++] = argp[i];
    stk[idx++] = 0;
    for (int i = 0; i < envc; i++) stk[idx++] = envpp[i];
    stk[idx++] = 0;
    /* auxv */
    for (int i = 0; i < naux; i++) {
        uint64_t key = aux[i].key;
        uint64_t val = aux[i].val;
        if (key == AT_RANDOM) val = at_random;
        stk[idx++] = key;
        stk[idx++] = val;
    }
    stk[idx++] = AT_NULL;
    stk[idx++] = 0;

    return (uint64_t)(uintptr_t)stk;
}

/* From vmm.c: build a fresh user address space sharing kernel mappings, and
 * tear an old one down (freeing its user frames). Used to give each exec a
 * clean address space and reclaim the previous image's pages (no per-exec
 * leak). */
extern uint64_t vmm_clone_kernel_pagetable(void);
extern void vmm_destroy_pagetable(uint64_t pml4_phys);
extern void vmm_switch_pagetable(uint64_t phys);

/* Apply R_X86_64_RELATIVE relocations from the PT_DYNAMIC RELA table of a PIE.
 * Runs in the target address space (t->cr3 is active), writing through the user
 * virtual addresses just mapped. */
static void apply_relocations(const elf64_ehdr_t *eh, const uint8_t *image,
                              uint64_t load_bias) {
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(image + eh->e_phoff);
    const elf64_dyn_t *dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (const elf64_dyn_t *)(image + ph[i].p_offset);
            break;
        }
    }
    if (!dyn) return;

    uint64_t rela_addr = 0, rela_sz = 0, rela_ent = sizeof(elf64_rela_t);
    for (const elf64_dyn_t *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA:    rela_addr = d->d_val; break;
        case DT_RELASZ:  rela_sz = d->d_val; break;
        case DT_RELAENT: rela_ent = d->d_val; break;
        default: break;
        }
    }
    if (!rela_addr || !rela_sz || !rela_ent) return;

    /* The RELA table address is a (bias-relative) virtual address now mapped. */
    uint64_t count = rela_sz / rela_ent;
    for (uint64_t i = 0; i < count; i++) {
        const elf64_rela_t *r =
            (const elf64_rela_t *)(uintptr_t)(load_bias + rela_addr + i * rela_ent);
        if (ELF64_R_TYPE(r->r_info) == R_X86_64_RELATIVE) {
            uint64_t *target = (uint64_t *)(uintptr_t)(load_bias + r->r_offset);
            *target = load_bias + (uint64_t)r->r_addend;
        }
    }
}

/* Load base for the dynamic loader (PT_INTERP). High enough to avoid the
 * program image (1 GiB), heap, and the mmap arena, but below the stack. */
#define INTERP_BASE 0x0000004000000000ULL   /* 256 GiB */

/* Load the program interpreter (e.g. /lib/ld-lariat.so.1) at `base` and return
 * its entry point. The loader is itself a PIE; we map its PT_LOAD segments at
 * `base` and let it self-relocate from its entry (it processes its own
 * R_X86_64_RELATIVE relocations), exactly like on Linux. */
static int load_interp(struct thread *t, const char *path, uint64_t base,
                       uint64_t *out_entry) {
    struct vfs_file *f = vfs_open(path, O_RDONLY);
    if (!f) return -ENOENT;
    uint32_t sz = f->inode ? f->inode->size : 0;
    if (sz == 0 || sz > (16u << 20)) { vfs_close(f); return -ENOEXEC; }
    uint8_t *buf = kmalloc(sz);
    if (!buf) { vfs_close(f); return -ENOMEM; }
    ssize_t n = vfs_read(f, buf, sz);
    vfs_close(f);
    if (n <= 0) { kfree(buf); return -ENOEXEC; }

    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)buf;
    if ((size_t)n < sizeof(*eh) || eh->e_magic != ELF_MAGIC) {
        kfree(buf);
        return -ENOEXEC;
    }
    const elf64_phdr_t *ph = (const elf64_phdr_t *)(buf + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t flags = (ph[i].p_flags & 0x2) ? PT_WRITABLE : 0;
        if (map_and_fill(t, ph[i].p_vaddr + base, buf, ph[i].p_offset,
                         ph[i].p_filesz, ph[i].p_memsz, flags) < 0) {
            kfree(buf);
            return -ENOMEM;
        }
    }
    *out_entry = eh->e_entry + base;
    kfree(buf);
    return 0;
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

    /* Acquire the program image.  Small binaries are read whole into a kernel
     * buffer (`image`); large ones are streamed segment-by-segment from the
     * still-open file (`stream_file`), buffering only the ELF header + program
     * headers (`hdrbuf`), so multi-megabyte executables like gcc's cc1 load
     * without a giant contiguous allocation or the old 16 MiB cap. */
    const uint8_t *image = NULL;          /* whole-file image (small/embedded) */
    size_t image_len = 0;
    uint8_t *heap_buf = NULL;             /* owns `image` when read from disk   */
    uint8_t *hdrbuf = NULL;               /* ELF header + phdrs (streaming)     */
    struct vfs_file *stream_file = NULL;  /* open image file (streaming)        */

    /* `hdr`/`hdr_len` point at the bytes holding the ELF header + phdrs: the
     * whole image for the small path, or hdrbuf for the streaming path. */
    const uint8_t *hdr = NULL;
    size_t hdr_len = 0;

    /* Binaries up to this size are read whole; larger ones stream from disk. */
#define ELF_STREAM_THRESHOLD (16u << 20)

    /* set-user/group-ID-on-exec state captured from the on-disk inode. */
    uint32_t exec_mode = 0, exec_uid = 0, exec_gid = 0;

    /* Resolve on-disk binaries first so packaged programs (Phase 2 / lpkg) can
     * add or override tools without rebuilding the kernel.  The embedded blobs
     * are only a fallback that guarantees a usable userland on an empty disk. */
    struct vfs_file *f = vfs_open(path, O_RDONLY);
    const struct embedded_prog *emb = f ? NULL : find_embedded(path);
    if (emb) {
        image = emb->start;
        image_len = (size_t)(emb->end - emb->start);
        hdr = image; hdr_len = image_len;
    } else {
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
        if (sz == 0) { vfs_close(f); return -ENOEXEC; }
        if (sz <= ELF_STREAM_THRESHOLD) {
            heap_buf = kmalloc(sz);
            if (!heap_buf) { vfs_close(f); return -ENOMEM; }
            ssize_t n = vfs_read(f, heap_buf, sz);
            vfs_close(f);
            if (n <= 0) { kfree(heap_buf); return -ENOEXEC; }
            image = heap_buf;
            image_len = (size_t)n;
            hdr = image; hdr_len = image_len;
        } else {
            /* Large image: buffer only the ELF header + program headers; the
             * PT_LOAD segments stream from `stream_file` later.  Such binaries
             * are dynamically linked (PT_INTERP present); a giant static PIE
             * has no in-kernel relocation path here and is rejected below. */
            elf64_ehdr_t ehtmp;
            if (vfs_read(f, &ehtmp, sizeof ehtmp) != (ssize_t)sizeof ehtmp ||
                ehtmp.e_magic != ELF_MAGIC) { vfs_close(f); return -ENOEXEC; }
            size_t phsz = (size_t)ehtmp.e_phoff +
                          (size_t)ehtmp.e_phnum * ehtmp.e_phentsize;
            if (phsz < sizeof ehtmp || phsz > (1u << 20)) {
                vfs_close(f); return -ENOEXEC;
            }
            hdrbuf = kmalloc(phsz);
            if (!hdrbuf) { vfs_close(f); return -ENOMEM; }
            if (vfs_lseek(f, 0, SEEK_SET) != 0 ||
                vfs_read(f, hdrbuf, phsz) != (ssize_t)phsz) {
                kfree(hdrbuf); vfs_close(f); return -ENOEXEC;
            }
            stream_file = f;          /* keep open for segment streaming */
            image_len = sz;
            hdr = hdrbuf; hdr_len = phsz;
        }
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

    /* Per-exec address-space teardown (no leak): when this thread is the sole
     * owner of its address space, give exec a brand-new page table and destroy
     * the old one afterwards, reclaiming the previous image's frames. For a
     * shared address space (CLONE_VM threads) fall back to the in-place reset. */
    uint64_t old_cr3 = 0;
    if (t->mm_count != NULL) {
        /* Shared address space (CLONE_VM - e.g. a posix_spawn/vfork child).
         * exec must give *this* thread a private image without disturbing the
         * other sharers: unshare into a brand-new page table, drop our
         * reference to the shared mm, and become its sole owner.  The old
         * shared cr3 is left intact for the remaining sharers (the parent
         * that spawned us is still running on it). */
        uint64_t newpt = vmm_clone_kernel_pagetable();
        if (newpt) {
            (*t->mm_count)--;          /* release our share of the old space */
            t->mm_count = NULL;        /* sole owner of the fresh space */
            t->cr3 = newpt;
            vmm_switch_pagetable(newpt);
        } else {
            /* Out of memory: nothing better than an in-place reset, which may
             * disturb co-sharers, but exec is already committed. */
            reset_address_space(t);
        }
    } else {
        uint64_t newpt = vmm_clone_kernel_pagetable();
        if (newpt) {
            old_cr3 = t->cr3;
            t->cr3 = newpt;
            vmm_switch_pagetable(newpt);   /* run on the fresh table */
        } else {
            reset_address_space(t);
        }
    }

    /* exec resets signal dispositions to the default and clears any pending or
     * blocked signals, so a new program never inherits the previous one's
     * handlers (e.g. the shell's SIGINT catcher). */
    for (int i = 0; i < 32; i++) t->sig_handlers[i] = 0;
    t->sig_pending = 0;
    t->sig_mask = 0;
    t->sig_restorer = 0;

    struct auxval aux[16];
    int naux = 0;

    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)hdr;
    if (hdr_len >= sizeof(elf64_ehdr_t) && eh->e_magic == ELF_MAGIC &&
        (eh->e_type == ET_EXEC || eh->e_type == ET_DYN)) {
        /* Proper ELF64 image. PIE (ET_DYN linked at 0) is loaded at a fixed
         * bias and base-relative-relocated; ET_EXEC keeps its link addresses. */
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(hdr + eh->e_phoff);
        uint64_t load_bias = 0;
        if (eh->e_type == ET_DYN) {
            uint64_t minv = ~0ULL;
            for (int i = 0; i < eh->e_phnum; i++)
                if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < minv)
                    minv = ph[i].p_vaddr;
            if (minv == 0) load_bias = USER_DYN_BASE;
        }
        entry = eh->e_entry + load_bias;

        for (int i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type != PT_LOAD) continue;
            uint64_t flags = (ph[i].p_flags & 0x2) ? PT_WRITABLE : 0;
            int rc = stream_file
                ? map_and_fill_file(t, ph[i].p_vaddr + load_bias, stream_file,
                                    ph[i].p_offset, ph[i].p_filesz, ph[i].p_memsz, flags)
                : map_and_fill(t, ph[i].p_vaddr + load_bias, image, ph[i].p_offset,
                               ph[i].p_filesz, ph[i].p_memsz, flags);
            if (rc < 0) {
                if (heap_buf) kfree(heap_buf);
                if (hdrbuf) kfree(hdrbuf);
                if (stream_file) vfs_close(stream_file);
                kfree(argbuf);
                if (old_cr3) vmm_destroy_pagetable(old_cr3);
                return -ENOMEM;
            }
        }

        uint64_t prog_entry = entry;   /* the program's own entry */

        /* PT_INTERP: a dynamically-linked executable names a loader (ld.so).
         * Load it and transfer control there; it relocates itself and the
         * program in userspace. Without an interpreter we apply the program's
         * RELATIVE relocations ourselves (static PIE). */
        const char *interp_path = NULL;
        char interpbuf[256];
        for (int i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type != PT_INTERP || !ph[i].p_filesz) continue;
            if (stream_file) {
                size_t l = ph[i].p_filesz;
                if (l > sizeof interpbuf - 1) l = sizeof interpbuf - 1;
                if (vfs_lseek(stream_file, (off_t)ph[i].p_offset, SEEK_SET)
                        == (off_t)ph[i].p_offset &&
                    vfs_read(stream_file, interpbuf, l) == (ssize_t)l) {
                    interpbuf[l] = '\0';
                    interp_path = interpbuf;
                }
            } else {
                interp_path = (const char *)(image + ph[i].p_offset);
            }
        }

        uint64_t at_base = load_bias;
        if (interp_path && load_interp(t, interp_path, INTERP_BASE, &entry) == 0) {
            at_base = INTERP_BASE;    /* its base, for self-relocation */
            /* program relocations are the loader's responsibility */
        } else if (!stream_file) {
            /* No usable interpreter: best-effort in-kernel self-relocation
             * (static PIE).  Only possible with the whole image in memory. */
            apply_relocations(eh, image, load_bias);
        } else {
            /* A streamed (large) image with no usable interpreter cannot be
             * self-relocated here (we don't hold the whole file). */
            kfree(hdrbuf);
            vfs_close(stream_file);
            kfree(argbuf);
            if (old_cr3) vmm_destroy_pagetable(old_cr3);
            return -ENOEXEC;
        }

        /* Locate the program headers in the loaded image for AT_PHDR. */
        uint64_t phdr_user = load_bias + eh->e_phoff;
        for (int i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type == PT_LOAD &&
                ph[i].p_offset <= eh->e_phoff &&
                eh->e_phoff < ph[i].p_offset + ph[i].p_filesz) {
                phdr_user = ph[i].p_vaddr + load_bias + (eh->e_phoff - ph[i].p_offset);
                break;
            }
        }

        aux[naux++] = (struct auxval){ AT_PHDR,   phdr_user };
        aux[naux++] = (struct auxval){ AT_PHENT,  eh->e_phentsize };
        aux[naux++] = (struct auxval){ AT_PHNUM,  eh->e_phnum };
        aux[naux++] = (struct auxval){ AT_BASE,   at_base };
        aux[naux++] = (struct auxval){ AT_ENTRY,  prog_entry };
        aux[naux++] = (struct auxval){ AT_FLAGS,  0 };
    } else {
        /* Flat binary: load verbatim at USER_CODE_START. (Only the small/
         * embedded path reaches here; streamed images are validated ELF.) */
        entry = USER_CODE_START;
        if (map_and_fill(t, USER_CODE_START, image, 0, image_len, image_len,
                         PT_WRITABLE) < 0) {
            if (heap_buf) kfree(heap_buf);
            if (hdrbuf) kfree(hdrbuf);
            if (stream_file) vfs_close(stream_file);
            kfree(argbuf);
            if (old_cr3) vmm_destroy_pagetable(old_cr3);
            return -ENOMEM;
        }
    }

    if (heap_buf) kfree(heap_buf);
    if (hdrbuf) kfree(hdrbuf);
    if (stream_file) vfs_close(stream_file);

    /* Apply set-user/group-ID-on-exec: a successful exec of a setuid binary
     * elevates the effective (and saved) IDs to the file owner's. */
    if (exec_mode & S_ISUID) { t->euid = exec_uid; t->suid = exec_uid; }
    if (exec_mode & S_ISGID) { t->egid = exec_gid; t->sgid = exec_gid; }

    /* Common auxv entries for both ELF and flat images. */
    aux[naux++] = (struct auxval){ AT_PAGESZ, PAGE_SIZE };
    aux[naux++] = (struct auxval){ AT_CLKTCK, 100 };
    aux[naux++] = (struct auxval){ AT_UID,    t->uid };
    aux[naux++] = (struct auxval){ AT_EUID,   t->euid };
    aux[naux++] = (struct auxval){ AT_GID,    t->gid };
    aux[naux++] = (struct auxval){ AT_EGID,   t->egid };
    aux[naux++] = (struct auxval){ AT_SECURE, 0 };
    aux[naux++] = (struct auxval){ AT_RANDOM, 0 };  /* filled on the stack */

    /* The old address space (and the previous image's frames) is no longer
     * needed; reclaim it now that the new image is fully loaded. */
    if (old_cr3) vmm_destroy_pagetable(old_cr3);

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

    uint64_t rsp = setup_user_stack(t, USER_STACK_TOP, kargv, kenvp, aux, naux);
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

