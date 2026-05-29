#include "kapi.h"
#include "serial.h"
#include "vga.h"
#include "idt.h"
#include "pic.h"
#include "pmm.h"
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

#define MAX_TRACKED_PAGES 32768  /* 128MB / 4KB */
#define PAGE_FREE    -1
#define PAGE_LARGE   -2

typedef struct free_obj {
    struct free_obj *next;
} free_obj_t;

typedef struct {
    free_obj_t *free_list;
    size_t      obj_size;
    size_t      objs_per_page;
} bucket_t;

static bucket_t buckets[NUM_BUCKETS];
static int8_t   page_bucket[MAX_TRACKED_PAGES];
static spinlock_t kmalloc_lock = SPINLOCK_INIT;

void kapi_heap_init(void *start, size_t size) {
    (void)start;
    (void)size;
    for (int i = 0; i < NUM_BUCKETS; i++) {
        buckets[i].free_list = NULL;
        buckets[i].obj_size = BUCKET_SIZE(i);
        buckets[i].objs_per_page = PAGE_SIZE / BUCKET_SIZE(i);
    }
    for (size_t i = 0; i < MAX_TRACKED_PAGES; i++) {
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

    char *page = (char *)phys;  /* identity mapped */
    size_t count = b->objs_per_page;
    uint64_t page_idx = phys / PAGE_SIZE;
    page_bucket[page_idx] = (int8_t)bi;

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
        ptr = (void *)pmm_alloc_pages(pages);
        if (ptr) {
            uint64_t page_idx = (uint64_t)ptr / PAGE_SIZE;
            for (size_t i = 0; i < pages && (page_idx + i) < MAX_TRACKED_PAGES; i++) {
                page_bucket[page_idx + i] = PAGE_LARGE;
            }
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
    uint64_t addr = (uint64_t)ptr;
    uint64_t page_idx = addr / PAGE_SIZE;

    if (page_idx >= MAX_TRACKED_PAGES) {
        spin_unlock_irqrestore(&kmalloc_lock, flags);
        return;
    }

    int8_t bi = page_bucket[page_idx];

    if (bi >= 0 && bi < NUM_BUCKETS) {
        /* Bucket allocation: return to free list */
        free_obj_t *obj = (free_obj_t *)addr;
        obj->next = buckets[bi].free_list;
        buckets[bi].free_list = obj;
    } else if (bi == PAGE_LARGE) {
        /* Large allocation: for now, single-page large allocs only */
        /* TODO: track page count for multi-page large allocations */
        pmm_free_page(addr);
        page_bucket[page_idx] = PAGE_FREE;
    }

    spin_unlock_irqrestore(&kmalloc_lock, flags);
}

/* --------------------------------------------------------------------------
 * Physical page allocation wrappers
 * -------------------------------------------------------------------------- */
void *alloc_pages(size_t count) {
    uint64_t phys = pmm_alloc_pages(count);
    return phys ? (void *)phys : NULL;
}

void free_pages(void *ptr, size_t count) {
    if (ptr) pmm_free_pages((uint64_t)ptr, count);
}

/* --------------------------------------------------------------------------
 * DMA allocation
 * -------------------------------------------------------------------------- */
void *dma_alloc(size_t size, uint64_t *phys_out) {
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) return NULL;
    if (phys_out) *phys_out = phys;
    return (void *)phys;
}

void dma_free(void *ptr, size_t size) {
    (void)size;
    if (ptr) pmm_free_page((uint64_t)ptr);
}

/* --------------------------------------------------------------------------
 * MMIO / I/O mapping
 * -------------------------------------------------------------------------- */
void *ioremap(uint64_t phys, size_t size) {
    (void)size;
    /* For now: identity mapped region, just return phys */
    /* TODO: proper page table mapping when we have VMM fully wired */
    return (void *)(uintptr_t)phys;
}

void iounmap(void *virt, size_t size) {
    (void)virt;
    (void)size;
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
