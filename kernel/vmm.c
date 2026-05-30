#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "serial.h"
#include "smp.h"

/* Direct map active flag (declared in mm.h). Starts identity. */
int g_physmap_active = 0;

/* Physical address of the kernel PML4. Boot loader builds it at 0x1000. */
static uint64_t kernel_pml4_phys = 0x1000;

/* Access a page-table page (identified by its physical address) through the
 * direct map. */
static inline uint64_t *pt_ptr(uint64_t phys) {
    return (uint64_t *)phys_to_virt(phys);
}

/* The kernel top-level table as a (direct-mapped) pointer. */
#define kernel_pml4 (pt_ptr(kernel_pml4_phys))

/* --------------------------------------------------------------------------
 * CR3 helpers
 * -------------------------------------------------------------------------- */
uint64_t vmm_get_cr3(void) {
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* Physical address of the kernel's PML4 (the address space kernel-only threads
 * run in).  Captured from the boot CR3 during vmm_init. */
uint64_t vmm_kernel_cr3(void) {
    return kernel_pml4_phys;
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

/* Allocate a zeroed page-table page; returns its physical address. */
static uint64_t alloc_pt_page(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;
    uint64_t *virt = pt_ptr(phys);
    for (int i = 0; i < 512; i++) {
        virt[i] = 0;
    }
    return phys;
}

/* --------------------------------------------------------------------------
 * Map a single 4KB page in a specific page table (pml4 is a direct-mapped ptr)
 * -------------------------------------------------------------------------- */
int vmm_map_page_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4i = pt_index(virt, 3);
    uint64_t pdpti = pt_index(virt, 2);
    uint64_t pdi   = pt_index(virt, 1);
    uint64_t pti   = pt_index(virt, 0);

    /* PML4 -> PDPT */
    if (!pt_is_present(pml4[pml4i])) {
        uint64_t pdpt_phys = alloc_pt_page();
        if (!pdpt_phys) return -1;
        pml4[pml4i] = pdpt_phys | PT_PRESENT | PT_WRITABLE | PT_USER;
    }
    uint64_t *pdpt = pt_ptr(pt_get_addr(pml4[pml4i]));

    /* PDPT -> PD */
    if (!pt_is_present(pdpt[pdpti])) {
        uint64_t pd_phys = alloc_pt_page();
        if (!pd_phys) return -1;
        pdpt[pdpti] = pd_phys | PT_PRESENT | PT_WRITABLE | PT_USER;
    }
    uint64_t *pd = pt_ptr(pt_get_addr(pdpt[pdpti]));

    /* PD -> PT (if current entry is a huge page, we can't map a small page) */
    if (pt_is_present(pd[pdi]) && pt_is_huge(pd[pdi])) {
        return -1;
    }

    if (!pt_is_present(pd[pdi])) {
        uint64_t pt_phys = alloc_pt_page();
        if (!pt_phys) return -1;
        pd[pdi] = pt_phys | PT_PRESENT | PT_WRITABLE | PT_USER;
    }
    uint64_t *pt = pt_ptr(pt_get_addr(pd[pdi]));

    /* Set the PTE */
    pt[pti] = (phys & PT_ADDR_MASK) | flags | PT_PRESENT;
    vmm_invlpg(virt);
    return 0;
}

/* --------------------------------------------------------------------------
 * Unmap a single 4KB page in a specific page table
 * -------------------------------------------------------------------------- */
void vmm_unmap_page_in(uint64_t *pml4, uint64_t virt) {
    uint64_t pml4i = pt_index(virt, 3);
    uint64_t pdpti = pt_index(virt, 2);
    uint64_t pdi   = pt_index(virt, 1);
    uint64_t pti   = pt_index(virt, 0);

    if (!pt_is_present(pml4[pml4i])) return;
    uint64_t *pdpt = pt_ptr(pt_get_addr(pml4[pml4i]));

    if (!pt_is_present(pdpt[pdpti])) return;
    uint64_t *pd = pt_ptr(pt_get_addr(pdpt[pdpti]));

    if (!pt_is_present(pd[pdi])) return;
    if (pt_is_huge(pd[pdi])) return;

    uint64_t *pt = pt_ptr(pt_get_addr(pd[pdi]));
    pt[pti] = 0;
    vmm_invlpg(virt);
}

/* --------------------------------------------------------------------------
 * Virtual to physical translation in a specific page table
 * -------------------------------------------------------------------------- */
uint64_t vmm_virt_to_phys_in(uint64_t *pml4, uint64_t virt) {
    uint64_t pml4i = pt_index(virt, 3);
    uint64_t pdpti = pt_index(virt, 2);
    uint64_t pdi   = pt_index(virt, 1);
    uint64_t pti   = pt_index(virt, 0);
    uint64_t offset = virt & 0xFFF;

    if (!pt_is_present(pml4[pml4i])) return 0;
    uint64_t *pdpt = pt_ptr(pt_get_addr(pml4[pml4i]));

    if (!pt_is_present(pdpt[pdpti])) return 0;
    uint64_t *pd = pt_ptr(pt_get_addr(pdpt[pdpti]));

    if (!pt_is_present(pd[pdi])) return 0;
    if (pt_is_huge(pd[pdi])) {
        return pt_get_addr(pd[pdi]) | (virt & 0x1FFFFF);
    }

    uint64_t *pt = pt_ptr(pt_get_addr(pd[pdi]));
    if (!pt_is_present(pt[pti])) return 0;
    return pt_get_addr(pt[pti]) | offset;
}

/* --------------------------------------------------------------------------
 * Kernel page table wrappers (backward compatible)
 * -------------------------------------------------------------------------- */
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    return vmm_map_page_in(kernel_pml4, virt, phys, flags);
}

void vmm_unmap_page(uint64_t virt) {
    vmm_unmap_page_in(kernel_pml4, virt);
    /* Kernel mappings are shared by every CPU, so other cores must flush. */
    smp_tlb_shootdown();
}

uint64_t vmm_virt_to_phys(uint64_t virt) {
    return vmm_virt_to_phys_in(kernel_pml4, virt);
}

/* --------------------------------------------------------------------------
 * Map/unmap ranges in a specific page table
 * -------------------------------------------------------------------------- */
int vmm_map_range_in(uint64_t *pml4, uint64_t virt, uint64_t phys, size_t count, uint64_t flags) {
    for (size_t i = 0; i < count; i++) {
        if (vmm_map_page_in(pml4, virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags) < 0) {
            for (size_t j = 0; j < i; j++) {
                vmm_unmap_page_in(pml4, virt + j * PAGE_SIZE);
            }
            return -1;
        }
    }
    return 0;
}

void vmm_unmap_range_in(uint64_t *pml4, uint64_t virt, size_t count) {
    for (size_t i = 0; i < count; i++) {
        vmm_unmap_page_in(pml4, virt + i * PAGE_SIZE);
    }
}

int vmm_map_range(uint64_t virt, uint64_t phys, size_t count, uint64_t flags) {
    return vmm_map_range_in(kernel_pml4, virt, phys, count, flags);
}

void vmm_unmap_range(uint64_t virt, size_t count) {
    vmm_unmap_range_in(kernel_pml4, virt, count);
    /* Kernel mappings are shared by every CPU, so other cores must flush. */
    smp_tlb_shootdown();
}

/* --------------------------------------------------------------------------
 * 2MB huge page mapping into the kernel PML4
 * -------------------------------------------------------------------------- */
static int map_huge_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4i = pt_index(virt, 3);
    uint64_t pdpti = pt_index(virt, 2);
    uint64_t pdi   = pt_index(virt, 1);

    if (!pt_is_present(kernel_pml4[pml4i])) {
        uint64_t pdpt_phys = alloc_pt_page();
        if (!pdpt_phys) return -1;
        kernel_pml4[pml4i] = pdpt_phys | PT_PRESENT | PT_WRITABLE;
    }
    uint64_t *pdpt = pt_ptr(pt_get_addr(kernel_pml4[pml4i]));

    if (!pt_is_present(pdpt[pdpti])) {
        uint64_t pd_phys = alloc_pt_page();
        if (!pd_phys) return -1;
        pdpt[pdpti] = pd_phys | PT_PRESENT | PT_WRITABLE;
    }
    uint64_t *pd = pt_ptr(pt_get_addr(pdpt[pdpti]));

    pd[pdi] = (phys & PT_ADDR_MASK) | flags | PT_PRESENT | PT_HUGE | PT_WRITABLE;
    vmm_invlpg(virt);
    return 0;
}

/* --------------------------------------------------------------------------
 * Build the direct map (physmap): linearly map all physical RAM at
 * PHYSMAP_BASE using 2MB huge pages.  Must run before any phys_to_virt() user
 * that touches memory above the boot identity window.
 * -------------------------------------------------------------------------- */
void vmm_build_physmap(void) {
    serial_print(SERIAL_COM1, "[VMM] Building direct map (physmap)...\n");

    uint64_t total_pages = pmm_total_pages();
    uint64_t total_2mb = (total_pages + 511) / 512;

    /* While building, phys_to_virt() is still identity, so the page-table
     * pages we allocate (from low RAM) and the kernel PML4 are reachable. */
    uint64_t mapped = 0;
    for (uint64_t i = 0; i < total_2mb; i++) {
        uint64_t phys = i * 0x200000;
        if (map_huge_page(PHYSMAP_BASE + phys, phys, 0) < 0) {
            serial_printf(SERIAL_COM1, "[VMM] physmap: failed at %x\n", phys);
            break;
        }
        mapped++;
    }

    g_physmap_active = 1;
    serial_printf(SERIAL_COM1,
        "[VMM] Direct map active: %d MB at %x\n",
        (int)(mapped * 2), (uint64_t)PHYSMAP_BASE);
}

/* --------------------------------------------------------------------------
 * Identity mapping with 2MB huge pages (transitional safety net for any code
 * that still dereferences low physical addresses directly).
 * -------------------------------------------------------------------------- */
void vmm_expand_identity_mapping(void) {
    serial_print(SERIAL_COM1, "[VMM] Expanding identity mapping...\n");

    uint64_t total_pages = pmm_total_pages();
    uint64_t total_2mb = (total_pages + 511) / 512;

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
    kernel_pml4_phys = vmm_get_cr3();
    serial_printf(SERIAL_COM1, "[VMM] Boot PML4 at %x\n", kernel_pml4_phys);
}

uint64_t vmm_new_pagetable(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;
    uint64_t *pml4 = pt_ptr(phys);
    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
    }
    return phys;
}

/* --------------------------------------------------------------------------
 * Per-process address spaces share the kernel's page tables *by reference* and
 * deep-copy only user pages.  This is essential for correctness: the kernel's
 * page-table pages (low identity in PML4[0]/PDPT[0] and the physmap + ioremap
 * mappings in PML4[256]) are shared by every address space, so they must never
 * be duplicated (a waste of RAM) nor freed when a process exits (which would
 * corrupt the live kernel mappings - e.g. the LAPIC).  Sharing is detected by
 * walking in parallel with the live kernel PML4: any entry bit-identical to the
 * kernel's at the same slot is a shared kernel entry.
 * -------------------------------------------------------------------------- */
static void copy_page_via_physmap(uint64_t dst_phys, uint64_t src_phys) {
    uint8_t *d = (uint8_t *)phys_to_virt(dst_phys);
    uint8_t *s = (uint8_t *)phys_to_virt(src_phys);
    for (int i = 0; i < PAGE_SIZE; i++) d[i] = s[i];
}

static void free_user_table(uint64_t phys, uint64_t kphys, int level);

/* Clone a sub-tree of a user address space.  Entries identical to the kernel's
 * (kphys, same slot) are shared verbatim and not descended into; everything
 * else is private and deep-copied (user leaf pages get fresh frames). */
static uint64_t clone_user_table(uint64_t src_phys, uint64_t kphys, int level) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;
    uint64_t *dst = pt_ptr(phys);
    uint64_t *src = pt_ptr(src_phys);
    uint64_t *k   = kphys ? pt_ptr(kphys) : 0;
    for (int i = 0; i < 512; i++) dst[i] = 0;

    for (int i = 0; i < 512; i++) {
        if (!pt_is_present(src[i])) continue;

        /* Shared kernel entry: copy by reference, do not descend. */
        if (k && pt_is_present(k[i]) &&
            pt_get_addr(k[i]) == pt_get_addr(src[i])) {
            dst[i] = src[i];
            continue;
        }

        uint64_t flags = pt_get_flags(src[i]);
        uint64_t addr  = pt_get_addr(src[i]);

        if (level == 0) {
            uint64_t np = pmm_alloc_page();
            if (!np) { free_user_table(phys, kphys, level); return 0; }
            copy_page_via_physmap(np, addr);
            dst[i] = np | flags;
        } else if (pt_is_huge(src[i])) {
            dst[i] = addr | flags;            /* shared backing frame */
        } else {
            uint64_t child = clone_user_table(addr, 0, level - 1);
            if (!child) { free_user_table(phys, kphys, level); return 0; }
            dst[i] = child | flags;
        }
    }
    return phys;
}

/* --------------------------------------------------------------------------
 * Clone the current address space for fork(): kernel mappings shared, user
 * pages deep-copied.
 * -------------------------------------------------------------------------- */
uint64_t vmm_clone_pagetable(uint64_t *src_pml4) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;
    uint64_t *dst = pt_ptr(phys);
    uint64_t *k = kernel_pml4;
    for (int i = 0; i < 512; i++) dst[i] = 0;

    for (int i = 0; i < 512; i++) {
        if (!pt_is_present(src_pml4[i])) continue;

        /* Whole subtree shared with the kernel (e.g. physmap): share by ref. */
        if (pt_is_present(k[i]) &&
            pt_get_addr(k[i]) == pt_get_addr(src_pml4[i])) {
            dst[i] = src_pml4[i];
            continue;
        }

        uint64_t kaddr = pt_is_present(k[i]) ? pt_get_addr(k[i]) : 0;
        uint64_t child = clone_user_table(pt_get_addr(src_pml4[i]), kaddr, 2);
        if (!child) {
            vmm_destroy_pagetable(phys);
            return 0;
        }
        dst[i] = child | pt_get_flags(src_pml4[i]);
    }
    return phys;
}

/* --------------------------------------------------------------------------
 * Build a fresh address space that shares the kernel's mappings.  PML4[0] is a
 * mixed slot (kernel low identity in PDPT[0] plus room for user mappings), so
 * it gets a private PDPT that references the kernel's low entries by value; all
 * other present kernel PML4 slots (physmap / ioremap) are shared verbatim.
 * -------------------------------------------------------------------------- */
uint64_t vmm_clone_kernel_pagetable(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;
    uint64_t *dst = pt_ptr(phys);
    uint64_t *k = kernel_pml4;
    for (int i = 0; i < 512; i++) dst[i] = 0;

    for (int i = 0; i < 512; i++) {
        if (!pt_is_present(k[i])) continue;
        if (i == 0) {
            uint64_t pdpt = pmm_alloc_page();
            if (!pdpt) { vmm_destroy_pagetable(phys); return 0; }
            uint64_t *dp = pt_ptr(pdpt);
            uint64_t *kp = pt_ptr(pt_get_addr(k[0]));
            for (int j = 0; j < 512; j++)
                dp[j] = pt_is_present(kp[j]) ? kp[j] : 0;
            dst[0] = pdpt | PT_PRESENT | PT_WRITABLE | PT_USER;
        } else {
            dst[i] = k[i];                    /* shared kernel slot */
        }
    }
    return phys;
}

/* --------------------------------------------------------------------------
 * Free a user address space produced by the clone helpers above.  Walks in
 * parallel with the kernel PML4 so that shared kernel page-table pages and
 * backing frames are never freed; only private per-process tables and user
 * leaf pages are returned to the PMM.
 * -------------------------------------------------------------------------- */
static void free_user_table(uint64_t phys, uint64_t kphys, int level) {
    uint64_t *t = pt_ptr(phys);
    uint64_t *k = kphys ? pt_ptr(kphys) : 0;
    for (int i = 0; i < 512; i++) {
        if (!pt_is_present(t[i])) continue;
        /* Shared with the kernel at this slot: leave it (and its subtree). */
        if (k && pt_is_present(k[i]) &&
            pt_get_addr(k[i]) == pt_get_addr(t[i])) continue;
        if (pt_is_huge(t[i])) continue;       /* shared/identity backing frame */
        if (level == 0) {
            pmm_free_page(pt_get_addr(t[i])); /* private user page */
        } else {
            free_user_table(pt_get_addr(t[i]), 0, level - 1);
        }
    }
    pmm_free_page(phys);
}

void vmm_destroy_pagetable(uint64_t pml4_phys) {
    if (!pml4_phys) return;
    uint64_t *pml4 = pt_ptr(pml4_phys);
    uint64_t *k = kernel_pml4;
    for (int i = 0; i < 512; i++) {
        if (!pt_is_present(pml4[i])) continue;
        uint64_t addr = pt_get_addr(pml4[i]);
        /* Entire subtree shared with the live kernel PML4: never free it. */
        if (pt_is_present(k[i]) && pt_get_addr(k[i]) == addr) continue;
        uint64_t kaddr = pt_is_present(k[i]) ? pt_get_addr(k[i]) : 0;
        free_user_table(addr, kaddr, 2);
    }
    pmm_free_page(pml4_phys);
}

/* --------------------------------------------------------------------------
 * Switch page table
 * -------------------------------------------------------------------------- */
void vmm_switch_pagetable(uint64_t phys) {
    vmm_set_cr3(phys);
}
