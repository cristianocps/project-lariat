#include "pmm.h"
#include "serial.h"
#include "vga.h"

/* --------------------------------------------------------------------------
 * PMM state
 * -------------------------------------------------------------------------- */
static uint8_t *bitmap = NULL;
static uint64_t bitmap_size = 0;     /* in bytes */
static uint64_t total_pages = 0;
static uint64_t free_page_count = 0;
static uint64_t highest_page = 0;

/* Linker symbol: end of kernel BSS */
extern char __bss_end[];

/* --------------------------------------------------------------------------
 * Bitmap helpers
 * -------------------------------------------------------------------------- */
static inline void bitmap_set(uint64_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bitmap_clear(uint64_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int bitmap_test(uint64_t page) {
    return (bitmap[page / 8] >> (page % 8)) & 1;
}

/* --------------------------------------------------------------------------
 * Mark regions
 * -------------------------------------------------------------------------- */
void pmm_mark_used(uint64_t phys, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint64_t page = pmm_phys_to_page(phys + (i * PAGE_SIZE));
        if (page < total_pages && !bitmap_test(page)) {
            bitmap_set(page);
            if (free_page_count > 0) free_page_count--;
        }
    }
}

void pmm_mark_free(uint64_t phys, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint64_t page = pmm_phys_to_page(phys + (i * PAGE_SIZE));
        if (page < total_pages && bitmap_test(page)) {
            bitmap_clear(page);
            free_page_count++;
        }
    }
}

/* --------------------------------------------------------------------------
 * Allocate / Free
 * -------------------------------------------------------------------------- */
uint64_t pmm_alloc_page(void) {
    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_page_count--;
            return pmm_page_to_phys(i);
        }
    }
    return 0;  /* Out of memory */
}

uint64_t pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;

    uint64_t consecutive = 0;
    uint64_t start = 0;

    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) start = i;
            consecutive++;
            if (consecutive >= count) {
                for (uint64_t j = start; j < start + count; j++) {
                    bitmap_set(j);
                }
                free_page_count -= count;
                return pmm_page_to_phys(start);
            }
        } else {
            consecutive = 0;
        }
    }
    return 0;  /* No contiguous block found */
}

void pmm_free_page(uint64_t phys) {
    uint64_t page = pmm_phys_to_page(phys);
    if (page < total_pages && bitmap_test(page)) {
        bitmap_clear(page);
        free_page_count++;
    }
}

void pmm_free_pages(uint64_t phys, size_t count) {
    for (size_t i = 0; i < count; i++) {
        pmm_free_page(phys + (i * PAGE_SIZE));
    }
}

/* --------------------------------------------------------------------------
 * Init from e820
 * -------------------------------------------------------------------------- */
void pmm_init_e820(uint64_t entry_count, e820_entry_t *entries) {
    serial_print(SERIAL_COM1, "[PMM] Parsing e820 map...\n");

    /* Sanity check: validate e820 data looks reasonable */
    int valid = 1;
    if (entry_count == 0 || entry_count > E820_MAX_ENTRIES) {
        valid = 0;
    } else if (entries[0].base != 0) {
        valid = 0;
    }

    if (!valid) {
        serial_printf(SERIAL_COM1,
            "[PMM] Invalid e820 data (%d entries), using fallback 32MB\n",
            (int)entry_count);
        entry_count = 2;
        static e820_entry_t fallback[2];
        fallback[0].base = 0;
        fallback[0].length = 0xA0000;
        fallback[0].type = E820_TYPE_AVAILABLE;
        fallback[1].base = 0x100000;
        fallback[1].length = 0x1F00000;
        fallback[1].type = E820_TYPE_AVAILABLE;
        entries = fallback;
    }

    /* Find highest address to size the bitmap */
    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < entry_count; i++) {
        uint64_t end = entries[i].base + entries[i].length;
        if (end > highest_addr) highest_addr = end;
    }

    /* Cap at 128MB to avoid huge bitmaps from high reserved regions */
    if (highest_addr > 0x8000000ULL) {
        serial_printf(SERIAL_COM1,
            "[PMM] High reserved regions detected, capping to 128 MB\n");
        highest_addr = 0x8000000ULL;
    }

    highest_page = pmm_phys_to_page(highest_addr);
    total_pages = highest_page + 1;
    bitmap_size = (total_pages + 7) / 8;

    serial_printf(SERIAL_COM1,
        "[PMM] Total RAM: %d MB (%d pages), bitmap: %d bytes\n",
        (int)(highest_addr / (1024 * 1024)),
        (int)total_pages,
        (int)bitmap_size);

    /* Place bitmap right after kernel BSS, page-aligned */
    bitmap = (uint8_t *)pmm_align_up((uint64_t)__bss_end);

    /* Zero the bitmap (mark all pages as used initially) */
    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
    }
    free_page_count = 0;

    /* Mark available regions from e820 as free */
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i].type == E820_TYPE_AVAILABLE && entries[i].length > 0) {
            uint64_t start = pmm_align_up(entries[i].base);
            uint64_t end = pmm_align_down(entries[i].base + entries[i].length);
            if (end > start) {
                uint64_t pages = (end - start) / PAGE_SIZE;
                pmm_mark_free(start, pages);
            }
        }
    }

    /* Mark kernel region as used (0x100000 to __bss_end) */
    uint64_t kernel_start = 0x100000;
    uint64_t kernel_end = pmm_align_up((uint64_t)__bss_end);
    pmm_mark_used(kernel_start, (kernel_end - kernel_start) / PAGE_SIZE);

    /* Mark page 0 as used (real mode IDT, BIOS data) */
    pmm_mark_used(0x0000, 1);

    /* Mark bootloader page tables as used (0x1000 - 0x3FFF) */
    pmm_mark_used(0x1000, 3);  /* 3 pages: PML4, PDPT, PD */

    /* Mark e820 buffer and count as used (0x4000 - 0x66FF) */
    pmm_mark_used(0x4000, 1);  /* 0x4000-0x4FFF */
    pmm_mark_used(0x5000, 1);  /* 0x5000-0x5FFF */
    pmm_mark_used(0x6000, 2);  /* 0x6000-0x7FFF (e820 buffer + padding) */

    /* Mark boot sector and stack area as used (0x7C00 - 0x9FFFF) */
    pmm_mark_used(0x7000, 1);  /* 0x7000-0x7FFF covers boot sector */

    /* Mark stack region as used (0x300000 - 0x30FFFF, 64KB stack) */
    pmm_mark_used(0x300000, 16);

    /* Mark the bitmap pages themselves as used */
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_mark_used((uint64_t)bitmap, bitmap_pages);

    serial_printf(SERIAL_COM1,
        "[PMM] Initialized: %d pages free / %d total\n",
        (int)free_page_count, (int)total_pages);
}

/* --------------------------------------------------------------------------
 * Debug / info
 * -------------------------------------------------------------------------- */
void pmm_dump_map(void) {
    serial_print(SERIAL_COM1, "[PMM] e820 memory map:\n");
    /* This is a stub - the e820 entries aren't stored globally yet */
}

uint64_t pmm_total_pages(void)     { return total_pages; }
uint64_t pmm_get_free_count(void) { return free_page_count; }
