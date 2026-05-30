#include "kapi.h"
#include "serial.h"
#include "vga.h"
#include "idt.h"
#include "pic.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include <stdarg.h>

/* --------------------------------------------------------------------------
 * IRQ management
 * -------------------------------------------------------------------------- */

typedef struct {
    irq_handler_t handler;
    void         *ctx;
    const char   *name;
    int           active;
} irq_entry_t;

static irq_entry_t irq_table[16];  /* PIC IRQs 0-15 */
static spinlock_t  irq_lock = SPINLOCK_INIT;

static void kapi_irq_dispatch(registers_t *regs) {
    uint8_t irq = (uint8_t)(regs->int_no - 32);
    if (irq < 16 && irq_table[irq].active) {
        irq_table[irq].handler(regs, irq_table[irq].ctx);
    }
    pic_send_eoi(irq);
}

int request_irq(uint8_t irq, irq_handler_t handler, void *ctx, const char *name) {
    if (irq >= 16 || !handler) return -1;

    uint64_t flags = spin_lock_irqsave(&irq_lock);

    if (irq_table[irq].active) {
        spin_unlock_irqrestore(&irq_lock, flags);
        return -1;  /* Already taken */
    }

    irq_table[irq].handler = handler;
    irq_table[irq].ctx     = ctx;
    irq_table[irq].name    = name;
    irq_table[irq].active  = 1;

    register_interrupt_handler(irq + 32, kapi_irq_dispatch);
    pic_clear_mask(irq);

    spin_unlock_irqrestore(&irq_lock, flags);

    KAPI_INFO("IRQ %d registered: %s\n", irq, name ? name : "?");
    return 0;
}

void free_irq(uint8_t irq) {
    if (irq >= 16) return;

    uint64_t flags = spin_lock_irqsave(&irq_lock);

    irq_table[irq].active  = 0;
    irq_table[irq].handler = NULL;
    irq_table[irq].ctx     = NULL;

    pic_set_mask(irq);

    spin_unlock_irqrestore(&irq_lock, flags);
}

/* --------------------------------------------------------------------------
 * Bucket allocator for kmalloc
 * -------------------------------------------------------------------------- */

#define MIN_BLOCK_SIZE   16
#define MAX_BLOCK_SIZE   2048
#define NUM_BUCKETS      8   /* 16, 32, 64, 128, 256, 512, 1024, 2048 */
#define BUCKET_INDEX(s)  (((s) <= 16) ? 0 : \
                          ((s) <= 32) ? 1 : \
                          ((s) <= 64) ? 2 : \
                          ((s) <= 128) ? 3 : \
                          ((s) <= 256) ? 4 : \
                          ((s) <= 512) ? 5 : \
                          ((s) <= 1024) ? 6 : 7)
#define BUCKET_SIZE(i)   (16 << (i))

/* page_bucket[] markers (>= 0 means "bucket index of this page"). */
#define PAGE_FREE        -1
#define PAGE_LARGE       -2   /* first page of a multi-page large allocation */
#define PAGE_LARGE_CONT  -3   /* continuation page of a large allocation */

typedef struct free_obj {
    struct free_obj *next;
} free_obj_t;

typedef struct {
    free_obj_t *free_list;
    size_t      obj_size;
    size_t      objs_per_page;
} bucket_t;

static bucket_t buckets[NUM_BUCKETS];

/* Per-physical-page ownership table, sized dynamically to total RAM and
 * allocated from the PMM (no more fixed 128MB ceiling). */
static int8_t    *page_bucket   = NULL;
static uint64_t   tracked_pages = 0;
static spinlock_t kmalloc_lock = SPINLOCK_INIT;

void kapi_heap_init(void *start, size_t size) {
    (void)start;
    (void)size;
    for (int i = 0; i < NUM_BUCKETS; i++) {
        buckets[i].free_list = NULL;
        buckets[i].obj_size = BUCKET_SIZE(i);
        buckets[i].objs_per_page = PAGE_SIZE / BUCKET_SIZE(i);
    }

    /* Allocate the page ownership table from the PMM, one byte per page. */
    tracked_pages = pmm_total_pages();
    uint64_t bytes = tracked_pages;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    page_bucket = (int8_t *)phys_to_virt(phys);
    for (uint64_t i = 0; i < tracked_pages; i++) {
        page_bucket[i] = PAGE_FREE;
    }

    kmalloc_lock.locked = 0;
}

static void *alloc_from_bucket(int bi) {
    bucket_t *b = &buckets[bi];

    if (b->free_list) {
        free_obj_t *obj = b->free_list;
        b->free_list = obj->next;
        return obj;
    }

    /* Allocate a new page and split into objects */
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;

    char *page = (char *)phys_to_virt(phys);
    size_t count = b->objs_per_page;
    uint64_t page_idx = phys / PAGE_SIZE;
    if (page_idx < tracked_pages) page_bucket[page_idx] = (int8_t)bi;

    /* Link all objects into free list, chaining onto existing list */
    for (size_t i = 0; i < count - 1; i++) {
        free_obj_t *obj = (free_obj_t *)(page + i * b->obj_size);
        obj->next = (free_obj_t *)(page + (i + 1) * b->obj_size);
    }
    free_obj_t *last = (free_obj_t *)(page + (count - 1) * b->obj_size);
    last->next = b->free_list;

    b->free_list = (free_obj_t *)page;

    /* Return first object, unlink it */
    free_obj_t *result = b->free_list;
    b->free_list = result->next;
    return result;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    uint64_t flags = spin_lock_irqsave(&kmalloc_lock);
    void *ptr;

    if (size > MAX_BLOCK_SIZE) {
        size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t phys = pmm_alloc_pages(pages);
        if (phys) {
            ptr = phys_to_virt(phys);
            uint64_t page_idx = phys / PAGE_SIZE;
            for (size_t i = 0; i < pages && (page_idx + i) < tracked_pages; i++) {
                page_bucket[page_idx + i] = (i == 0) ? PAGE_LARGE : PAGE_LARGE_CONT;
            }
        } else {
            ptr = NULL;
        }
    } else {
        int bi = BUCKET_INDEX(size);
        ptr = alloc_from_bucket(bi);
    }

    spin_unlock_irqrestore(&kmalloc_lock, flags);
    return ptr;
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) {
        uint8_t *p = ptr;
        for (size_t i = 0; i < size; i++) {
            p[i] = 0;
        }
    }
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;

    uint64_t flags = spin_lock_irqsave(&kmalloc_lock);
    uint64_t phys = virt_to_phys(ptr);
    uint64_t page_idx = phys / PAGE_SIZE;

    if (page_idx >= tracked_pages) {
        spin_unlock_irqrestore(&kmalloc_lock, flags);
        return;
    }

    int8_t bi = page_bucket[page_idx];

    if (bi >= 0 && bi < NUM_BUCKETS) {
        /* Bucket allocation: return to free list */
        free_obj_t *obj = (free_obj_t *)ptr;
        obj->next = buckets[bi].free_list;
        buckets[bi].free_list = obj;
    } else if (bi == PAGE_LARGE) {
        /* Large allocation: free the head page plus all continuation pages. */
        uint64_t i = page_idx;
        page_bucket[i] = PAGE_FREE;
        pmm_free_page(pmm_page_to_phys(i));
        i++;
        while (i < tracked_pages && page_bucket[i] == PAGE_LARGE_CONT) {
            page_bucket[i] = PAGE_FREE;
            pmm_free_page(pmm_page_to_phys(i));
            i++;
        }
    }

    spin_unlock_irqrestore(&kmalloc_lock, flags);
}

/* --------------------------------------------------------------------------
 * Physical page allocation wrappers
 * -------------------------------------------------------------------------- */
void *alloc_pages(size_t count) {
    uint64_t phys = pmm_alloc_pages(count);
    return phys ? phys_to_virt(phys) : NULL;
}

void free_pages(void *ptr, size_t count) {
    if (ptr) pmm_free_pages(virt_to_phys(ptr), count);
}

/* --------------------------------------------------------------------------
 * DMA allocation
 * -------------------------------------------------------------------------- */
void *dma_alloc(size_t size, uint64_t *phys_out) {
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) return NULL;
    if (phys_out) *phys_out = phys;
    return phys_to_virt(phys);
}

void dma_free(void *ptr, size_t size) {
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (ptr) pmm_free_pages(virt_to_phys(ptr), pages);
}

/* --------------------------------------------------------------------------
 * MMIO / I/O mapping
 *
 * Map a physical (typically device MMIO) range into the kernel's direct-map
 * window with caching disabled, and return a usable virtual pointer.  RAM is
 * already covered by the physmap; this also handles MMIO ranges above RAM.
 * -------------------------------------------------------------------------- */
void *ioremap(uint64_t phys, size_t size) {
    uint64_t start = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = (phys + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        vmm_map_page((uint64_t)PHYSMAP_BASE + p, p, PT_WRITABLE | PT_NOCACHE);
    }
    return (void *)((uint64_t)PHYSMAP_BASE + phys);
}

void iounmap(void *virt, size_t size) {
    uint64_t start = (uint64_t)virt & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end   = ((uint64_t)virt + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t v = start; v < end; v += PAGE_SIZE) {
        vmm_unmap_page(v);
    }
}

/* --------------------------------------------------------------------------
 * Spinlocks
 * -------------------------------------------------------------------------- */
void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        __asm__ __volatile__("pause");
    }
}

void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(&lock->locked);
}

uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags;
    __asm__ __volatile__(
        "pushfq\n"
        "pop %0\n"
        "cli"
        : "=r"(flags)
    );
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    __asm__ __volatile__(
        "push %0\n"
        "popfq"
        :: "r"(flags)
    );
}

/* --------------------------------------------------------------------------
 * Logging
 * -------------------------------------------------------------------------- */
void kapi_log(const char *level, const char *fmt, ...) {
    serial_printf(SERIAL_COM1, "[%s] ", level);

    va_list args;
    va_start(args, fmt);
    serial_vprintf(SERIAL_COM1, fmt, args);
    va_end(args);
}

/* --------------------------------------------------------------------------
 * String helpers (missing from freestanding libc)
 * -------------------------------------------------------------------------- */
int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = s1;
    const unsigned char *p2 = s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i])
            return (int)p1[i] - (int)p2[i];
    }
    return 0;
}
