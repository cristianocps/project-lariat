#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"

/* --------------------------------------------------------------------------
 * x86_64 page table flags
 * -------------------------------------------------------------------------- */
#define PT_PRESENT     (1 << 0)
#define PT_WRITABLE    (1 << 1)
#define PT_USER        (1 << 2)
#define PT_WRITETHROUGH (1 << 3)
#define PT_NOCACHE     (1 << 4)
#define PT_ACCESSED    (1 << 5)
#define PT_DIRTY       (1 << 6)
#define PT_HUGE        (1 << 7)
#define PT_GLOBAL      (1 << 8)
#define PT_NX          (1ULL << 63)

#define PT_FLAGS_MASK  0x8000000000000FFFULL
#define PT_ADDR_MASK   ~PT_FLAGS_MASK

/* Recursive mapping index (used for self-referencing page tables) */
#define PT_RECURSIVE_IDX  510

/* --------------------------------------------------------------------------
 * Page table entry types
 * -------------------------------------------------------------------------- */
typedef uint64_t pml4e_t;
typedef uint64_t pdpte_t;
typedef uint64_t pde_t;
typedef uint64_t pte_t;

/* --------------------------------------------------------------------------
 * VMM API
 * -------------------------------------------------------------------------- */

/* Get current CR3 (physical address of PML4) */
uint64_t vmm_get_cr3(void);

/* Set CR3 to a new PML4 physical address */
void vmm_set_cr3(uint64_t phys);

/* Invalidate TLB entry for a virtual address */
static inline void vmm_invlpg(uint64_t virt) {
    __asm__ __volatile__("invlpg (%0)" :: "r"(virt) : "memory");
}

/* --------------------------------------------------------------------------
 * Mapping / unmapping
 * -------------------------------------------------------------------------- */

/* Map a physical page to a virtual address with given flags */
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Unmap a virtual address */
void vmm_unmap_page(uint64_t virt);

/* Get physical address for a virtual address (0 if not mapped) */
uint64_t vmm_virt_to_phys(uint64_t virt);

/* Map a contiguous range */
int vmm_map_range(uint64_t virt, uint64_t phys, size_t count, uint64_t flags);
void vmm_unmap_range(uint64_t virt, size_t count);

/* --------------------------------------------------------------------------
 * Identity mapping expansion
 * -------------------------------------------------------------------------- */

/* Expand identity mapping to cover all RAM using 2MB huge pages */
void vmm_expand_identity_mapping(void);

/* Initialize VMM from current boot page tables */
void vmm_init(void);

/* Allocate a new top-level page table (PML4) */
uint64_t vmm_new_pagetable(void);

/* --------------------------------------------------------------------------
 * Page table entry helpers
 * -------------------------------------------------------------------------- */

static inline uint64_t pt_get_addr(uint64_t entry) {
    return entry & PT_ADDR_MASK;
}

static inline uint64_t pt_get_flags(uint64_t entry) {
    return entry & PT_FLAGS_MASK;
}

static inline int pt_is_present(uint64_t entry) {
    return (entry & PT_PRESENT) != 0;
}

static inline int pt_is_huge(uint64_t entry) {
    return (entry & PT_HUGE) != 0;
}

#endif
