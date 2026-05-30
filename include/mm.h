#ifndef MM_H
#define MM_H

#include <stdint.h>

/* --------------------------------------------------------------------------
 * Physical <-> virtual address translation.
 *
 * The kernel keeps a "direct map" (physmap) window that linearly maps all of
 * physical RAM at a fixed high canonical base.  Kernel code accesses physical
 * pages (page tables, kmalloc pages, DMA buffers) through phys_to_virt() rather
 * than dereferencing the raw physical address.  This decouples the kernel's
 * virtual addresses from physical addresses and is what allows supporting more
 * than the old identity-mapped window.
 *
 * Until the physmap is built early in kmain, phys_to_virt() is the identity
 * (the boot loader identity-maps the low 4MB), so early code keeps working.
 * -------------------------------------------------------------------------- */

#define PHYSMAP_BASE 0xFFFF800000000000ULL

/* Set to 1 by vmm_build_physmap() once the direct map is active. */
extern int g_physmap_active;

static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(g_physmap_active ? (phys + PHYSMAP_BASE) : phys);
}

static inline uint64_t virt_to_phys(const void *virt) {
    uint64_t a = (uint64_t)virt;
    /* Direct-map pointers live at/above PHYSMAP_BASE; everything else is a
     * low identity-mapped kernel address whose physical == virtual. */
    return (a >= PHYSMAP_BASE) ? (a - PHYSMAP_BASE) : a;
}

#endif /* MM_H */
