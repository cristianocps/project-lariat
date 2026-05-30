#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>

/* Userspace memory layout */
#define USER_CODE_START   0x0000000040000000ULL   /* 1 GB - above kernel identity map */
#define USER_STACK_TOP    0x0000007FC0000000ULL   /* high user address */
#define USER_STACK_SIZE   (4 * 4096)              /* 16 KB user stack */

struct thread;

/* Create a user process from a flat binary loaded in memory */
struct thread *process_create_flat(const uint8_t *code, size_t len);

/* Create a user process that loads an ELF/flat image by path on first dispatch
 * (uses the full ELF loader inside the new address space). */
struct thread *process_create_user(const char *path);

/* Fork the current user process */
struct thread *process_fork(struct thread *parent);

/* clone(2): spawn a thread/process from `parent`.  When CLONE_VM is set the new
 * thread shares the parent's address space (and a refcounted cr3); otherwise it
 * gets a copy like fork.  CLONE_FILES shares the fd table.  `new_stack` is the
 * top of the child's user stack (0 to inherit the parent's), `tls` the FS base
 * (CLONE_SETTLS), and `ptid`/`ctid` the parent/child tid write-back targets. */
struct thread *process_clone(struct thread *parent, uint64_t flags,
                             uint64_t new_stack, uint64_t tls,
                             int *ptid, int *ctid);

/* clone(2) flag bits (Linux ABI subset). */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* Enter ring-3 from kernel (does not return) */
void enter_userspace(uint64_t rip, uint64_t rsp);

#endif
