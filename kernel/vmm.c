#include "vmm.h"
#include "pmm.h"
#include "serial.h"

/* Current boot PML4 is at physical 0x1000 (identity mapped) */
static uint64_t *kernel_pml4 = (uint64_t *)0x1000;

/* --------------------------------------------------------------------------
 * CR3 helpers
 * -------------------------------------------------------------------------- */
uint64_t vmm_get_cr3(void) {
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void vmm_set_cr3(uint64_t phys) {
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(phys));
}

/* --------------------------------------------------------------------------
 * Page table walking helpers
 * -------------------------------------------------------------------------- */
static inline uint64_t pt_index(uint64_t virt, int level) {
    return (virt >> (12 + level * 9)) & 0x1FF;
}

/* Allocate a zeroed page table page */
static uint64_t *alloc_pt_page(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;
    uint64_t *virt = (uint64_t *)phys;  /* identity mapped */
    for (int i = 0; i < 512; i++) {
        virt[i] = 0;
    }
    return virt;
}

/* --------------------------------------------------------------------------
 * Map a single 4KB page
 * -------------------------------------------------------------------------- */
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4i = pt_index(virt, 3);
    uint64_t pdpti = pt_index(virt, 2);
    uint64_t pdi   = pt_index(virt, 1);
    uint64_t pti   = pt_index(virt, 0);

    /* PML4 -> PDPT */
    if (!pt_is_present(kernel_pml4[pml4i])) {
        uint64_t *pdpt = alloc_pt_page();
        if (!pdpt) return -1;
        kernel_pml4[pml4i] = (uint64_t)pdpt | PT_PRESENT | PT_WRITABLE;
    }
    uint64_t *pdpt = (uint64_t *)pt_get_addr(kernel_pml4[pml4i]);

    /* PDPT -> PD */
    if (!pt_is_present(pdpt[pdpti])) {
        uint64_t *pd = alloc_pt_page();
        if (!pd) return -1;
        pdpt[pdpti] = (uint64_t)pd | PT_PRESENT | PT_WRITABLE;
    }
    uint64_t *pd = (uint64_t *)pt_get_addr(pdpt[pdpti]);

    /* PD -> PT (if current entry is a huge page, we can't map a small page) */
    if (pt_is_present(pd[pdi]) && pt_is_huge(pd[pdi])) {
        /* Huge page already mapped here - error */
        return -1;
    }

    if (!pt_is_present(pd[pdi])) {
        uint64_t *pt = alloc_pt_page();
        if (!pt) return -1;
        pd[pdi] = (uint64_t)pt | PT_PRESENT | PT_WRITABLE;
    }
    uint64_t *pt = (uint64_t *)pt_get_addr(pd[pdi]);

    /* Set the PTE */
    pt[pti] = (phys & PT_ADDR_MASK) | flags | PT_PRESENT;
    vmm_invlpg(virt);
    return 0;
}

/* --------------------------------------------------------------------------
 * Unmap a single 4KB page
 * -------------------------------------------------------------------------- */
void vmm_unmap_page(uint64_t virt) {
    uint64_t pml4i = pt_index(virt, 3);
    uint64_t pdpti = pt_index(virt, 2);
    uint64_t pdi   = pt_index(virt, 1);
    uint64_t pti   = pt_index(virt, 0);

    if (!pt_is_present(kernel_pml4[pml4i])) return;
    uint64_t *pdpt = (uint64_t *)pt_get_addr(kernel_pml4[pml4i]);

    if (!pt_is_present(pdpt[pdpti])) return;
    uint64_t *pd = (uint64_t *)pt_get_addr(pdpt[pdpti]);

    if (!pt_is_present(pd[pdi])) return;
    if (pt_is_huge(pd[pdi])) return;  /* Can't unmap a huge page via this path */

    uint64_t *pt = (uint64_t *)pt_get_addr(pd[pdi]);
    pt[pti] = 0;
    vmm_invlpg(virt);
}

/* --------------------------------------------------------------------------
 * Virtual to physical translation
 * -------------------------------------------------------------------------- */
uint64_t vmm_virt_to_phys(uint64_t virt) {
    uint64_t pml4i = pt_index(virt, 3);
    uint64_t pdpti = pt_index(virt, 2);
    uint64_t pdi   = pt_index(virt, 1);
    uint64_t pti   = pt_index(virt, 0);
    uint64_t offset = virt & 0xFFF;

    if (!pt_is_present(kernel_pml4[pml4i])) return 0;
    uint64_t *pdpt = (uint64_t *)pt_get_addr(kernel_pml4[pml4i]);

    if (!pt_is_present(pdpt[pdpti])) return 0;
    uint64_t *pd = (uint64_t *)pt_get_addr(pdpt[pdpti]);

    if (!pt_is_present(pd[pdi])) return 0;
    if (pt_is_huge(pd[pdi])) {
        return pt_get_addr(pd[pdi]) | (virt & 0x1FFFFF);
    }

    uint64_t *pt = (uint64_t *)pt_get_addr(pd[pdi]);
    if (!pt_is_present(pt[pti])) return 0;
    return pt_get_addr(pt[pti]) | offset;
}

/* --------------------------------------------------------------------------
 * Map/unmap ranges
 * -------------------------------------------------------------------------- */
int vmm_map_range(uint64_t virt, uint64_t phys, size_t count, uint64_t flags) {
    for (size_t i = 0; i < count; i++) {
        if (vmm_map_page(virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags) < 0) {
            /* Rollback on failure */
            for (size_t j = 0; j < i; j++) {
                vmm_unmap_page(virt + j * PAGE_SIZE);
            }
            return -1;
        }
    }
    return 0;
}

void vmm_unmap_range(uint64_t virt, size_t count) {
    for (size_t i = 0; i < count; i++) {
        vmm_unmap_page(virt + i * PAGE_SIZE);
    }
}

/* --------------------------------------------------------------------------
 * Identity mapping with 2MB huge pages
 * -------------------------------------------------------------------------- */
static int map_huge_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4i = pt_index(virt, 3);
    uint64_t pdpti = pt_index(virt, 2);
    uint64_t pdi   = pt_index(virt, 1);

    /* PML4 -> PDPT */
    if (!pt_is_present(kernel_pml4[pml4i])) {
        uint64_t *pdpt = alloc_pt_page();
        if (!pdpt) return -1;
        kernel_pml4[pml4i] = (uint64_t)pdpt | PT_PRESENT | PT_WRITABLE;
    }
    uint64_t *pdpt = (uint64_t *)pt_get_addr(kernel_pml4[pml4i]);

    /* PDPT -> PD */
    if (!pt_is_present(pdpt[pdpti])) {
        uint64_t *pd = alloc_pt_page();
        if (!pd) return -1;
        pdpt[pdpti] = (uint64_t)pd | PT_PRESENT | PT_WRITABLE;
    }
    uint64_t *pd = (uint64_t *)pt_get_addr(pdpt[pdpti]);

    /* Set 2MB huge page entry */
    pd[pdi] = (phys & PT_ADDR_MASK) | flags | PT_PRESENT | PT_HUGE | PT_WRITABLE;
    vmm_invlpg(virt);
    return 0;
}

void vmm_expand_identity_mapping(void) {
    serial_print(SERIAL_COM1, "[VMM] Expanding identity mapping...\n");

    uint64_t total_pages = pmm_total_pages();
    uint64_t total_2mb = (total_pages + 511) / 512;  /* Round up to 2MB chunks */

    uint64_t mapped = 0;
    for (uint64_t i = 2; i < total_2mb; i++) {  /* Skip first 4MB already mapped */
        uint64_t phys = i * 0x200000;
        if (map_huge_page(phys, phys, 0) < 0) {
            serial_printf(SERIAL_COM1, "[VMM] Failed to map 2MB page at %x\n", phys);
            break;
        }
        mapped++;
    }

    serial_printf(SERIAL_COM1,
        "[VMM] Identity mapped %d additional 2MB pages (%d MB total)\n",
        (int)mapped, (int)((mapped + 2) * 2));
}

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */
void vmm_init(void) {
    kernel_pml4 = (uint64_t *)vmm_get_cr3();
    serial_printf(SERIAL_COM1, "[VMM] Boot PML4 at %x\n", (uint64_t)kernel_pml4);
}

uint64_t vmm_new_pagetable(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;
    uint64_t *pml4 = (uint64_t *)phys;
    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
    }
    return phys;
}
