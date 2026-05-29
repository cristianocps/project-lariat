#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

/* e820 entry from BIOS INT 0x15 AX=0xE820 */
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t extended;
} __attribute__((packed)) e820_entry_t;

#define E820_TYPE_AVAILABLE  1
#define E820_TYPE_RESERVED   2
#define E820_TYPE_ACPI       3
#define E820_TYPE_NVS        4
#define E820_TYPE_BAD        5

#define PAGE_SIZE      4096
#define PAGE_SHIFT     12
#define PAGE_MASK      (~(PAGE_SIZE - 1))

#define E820_MAX_ENTRIES 64

/* --------------------------------------------------------------------------
 * Physical Memory Manager API
 * -------------------------------------------------------------------------- */

/* Initialize from e820 map passed by bootloader */
void pmm_init_e820(uint64_t entry_count, e820_entry_t *entries);

/* Dump e820 map to serial */
void pmm_dump_map(void);

/* Total and free page counts */
uint64_t pmm_total_pages(void);
uint64_t pmm_get_free_count(void);

/* Allocate a single page (returns physical address, 0 on failure) */
uint64_t pmm_alloc_page(void);

/* Allocate contiguous pages (returns physical address, 0 on failure) */
uint64_t pmm_alloc_pages(size_t count);

/* Free pages back to the allocator */
void pmm_free_page(uint64_t phys);
void pmm_free_pages(uint64_t phys, size_t count);

/* Mark a physical page range as used (for reserved regions) */
void pmm_mark_used(uint64_t phys, size_t count);
void pmm_mark_free(uint64_t phys, size_t count);

/* Convert between page number and physical address */
static inline uint64_t pmm_page_to_phys(uint64_t page) {
    return page << PAGE_SHIFT;
}

static inline uint64_t pmm_phys_to_page(uint64_t phys) {
    return phys >> PAGE_SHIFT;
}

static inline uint64_t pmm_align_up(uint64_t addr) {
    return (addr + PAGE_SIZE - 1) & PAGE_MASK;
}

static inline uint64_t pmm_align_down(uint64_t addr) {
    return addr & PAGE_MASK;
}

#endif
