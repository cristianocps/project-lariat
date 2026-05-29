#ifndef KAPI_H
#define KAPI_H

#include <stdint.h>
#include <stddef.h>
#include "idt.h"

/* --------------------------------------------------------------------------
 * Kernel API for Drivers
 *
 * This is the stable interface that external / third-party drivers
 * compile against. We guarantee backward compatibility within a
 * major kernel version.
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * IRQ management
 * -------------------------------------------------------------------------- */

typedef void (*irq_handler_t)(registers_t *regs, void *ctx);

/* Request an IRQ line. Returns 0 on success, negative on error. */
int request_irq(uint8_t irq, irq_handler_t handler, void *ctx, const char *name);

/* Free a previously requested IRQ */
void free_irq(uint8_t irq);

/* --------------------------------------------------------------------------
 * Memory allocation (stubs until PMM is implemented)
 * -------------------------------------------------------------------------- */

/* Initialize the early bump allocator */
void kapi_heap_init(void *start, size_t size);

/* Physical page allocation */
void *alloc_pages(size_t count);
void  free_pages(void *ptr, size_t count);

/* General kernel heap */
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void  kfree(void *ptr);

/* DMA-coherent memory */
void *dma_alloc(size_t size, uint64_t *phys_out);
void  dma_free(void *ptr, size_t size);

/* --------------------------------------------------------------------------
 * MMIO / I/O mapping
 * -------------------------------------------------------------------------- */

/* Map a physical address range into kernel virtual address space */
void *ioremap(uint64_t phys, size_t size);
void  iounmap(void *virt, size_t size);

/* I/O port helpers - drivers should include <ports.h> directly */
#include "ports.h"

/* --------------------------------------------------------------------------
 * Synchronization primitives (stubs)
 * -------------------------------------------------------------------------- */

typedef struct {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);
uint64_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags);

/* --------------------------------------------------------------------------
 * Logging
 * -------------------------------------------------------------------------- */
void kapi_log(const char *level, const char *fmt, ...);

#define KAPI_INFO(fmt, ...)  kapi_log("INFO",  fmt, ##__VA_ARGS__)
#define KAPI_WARN(fmt, ...)  kapi_log("WARN",  fmt, ##__VA_ARGS__)
#define KAPI_ERR(fmt, ...)   kapi_log("ERROR", fmt, ##__VA_ARGS__)

/* --------------------------------------------------------------------------
 * Kernel version (for module compatibility checks)
 * -------------------------------------------------------------------------- */
#define LARIAT_VERSION_MAJOR 0
#define LARIAT_VERSION_MINOR 1
#define LARIAT_VERSION_PATCH 0

static inline uint32_t kapi_version(void) {
    return (LARIAT_VERSION_MAJOR << 16) |
           (LARIAT_VERSION_MINOR << 8)  |
           LARIAT_VERSION_PATCH;
}

#endif
