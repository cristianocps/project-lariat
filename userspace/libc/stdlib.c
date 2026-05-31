#include "stdlib.h"
#include "string.h"
#include "unistd.h"

/* --------------------------------------------------------------------------
 * Heap allocator.
 *
 * A first-fit free-list allocator backed by brk(2).  Blocks carry a header and
 * are kept in a single address-ordered doubly-linked list carved from one
 * contiguous brk region, so adjacent blocks coalesce on free and large blocks
 * split on allocation.  This replaces the previous bump allocator and supports
 * real free()/realloc(), which ported software (and a self-hosting toolchain)
 * depends on.  See docs/adr/0002-dynamic-linking-and-musl.md.
 * -------------------------------------------------------------------------- */

#define ALIGN16(x) (((x) + (size_t)15) & ~((size_t)15))

typedef struct block {
    size_t        size;   /* usable payload bytes (excludes header) */
    struct block *next;   /* next block in address order */
    struct block *prev;   /* previous block in address order */
    int           free;   /* 1 if available */
    int           _pad;
} block_t;

#define HDR ALIGN16(sizeof(block_t))   /* header size, 16-byte aligned */
#define HEAP_CHUNK (64 * 1024)         /* min brk growth per request */

static block_t *heap_head = 0;
static block_t *heap_tail = 0;
static unsigned long brk_cur = 0;

static int heap_init(void) {
    if (brk_cur) return 0;
    unsigned long b = (unsigned long)sbrk_set(0);
    if ((long)b <= 0) return -1;
    brk_cur = b;
    return 0;
}

/* Merge b with any immediately-following free blocks. */
static void coalesce(block_t *b) {
    while (b->next && b->next->free) {
        block_t *n = b->next;
        b->size += HDR + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        else heap_tail = b;
    }
}

/* Extend the break and append a new free block of at least `need` payload. */
static block_t *heap_grow(size_t need) {
    size_t total = HDR + need;
    size_t chunk = total < HEAP_CHUNK ? HEAP_CHUNK : total;
    unsigned long start = brk_cur;
    unsigned long newbrk = start + chunk;
    unsigned long got = (unsigned long)sbrk_set(newbrk);
    if (got < newbrk) return 0;
    brk_cur = newbrk;

    block_t *b = (block_t *)start;
    b->size = chunk - HDR;
    b->free = 1;
    b->next = 0;
    b->prev = heap_tail;
    if (heap_tail) heap_tail->next = b;
    else heap_head = b;
    heap_tail = b;
    return b;
}

void *malloc(size_t size) {
    if (heap_init() < 0) return 0;
    if (size == 0) size = 1;
    size = ALIGN16(size);

    block_t *b = heap_head;
    for (; b; b = b->next)
        if (b->free && b->size >= size) break;

    if (!b) {
        b = heap_grow(size);
        if (!b) return 0;
        /* If the previous block is free, fold the fresh region into it. */
        if (b->prev && b->prev->free) { coalesce(b->prev); b = b->prev; }
    }

    /* Split if the leftover can hold a header plus a minimal allocation. */
    if (b->size >= size + HDR + 16) {
        block_t *n = (block_t *)((char *)b + HDR + size);
        n->size = b->size - size - HDR;
        n->free = 1;
        n->next = b->next;
        n->prev = b;
        if (b->next) b->next->prev = n;
        else heap_tail = n;
        b->next = n;
        b->size = size;
    }
    b->free = 0;
    return (char *)b + HDR;
}

void *calloc(size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)-1 / nmemb) return 0;  /* overflow */
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return 0; }
    block_t *b = (block_t *)((char *)ptr - HDR);
    size_t want = ALIGN16(size);
    if (b->size >= want) return ptr;   /* shrink/no-op: keep block */

    /* Try to grow in place by absorbing the following free block. */
    if (b->next && b->next->free && b->size + HDR + b->next->size >= want) {
        coalesce(b);
        if (b->size >= want) return ptr;
    }

    void *n = malloc(size);
    if (!n) return 0;
    memcpy(n, ptr, b->size < size ? b->size : size);
    free(ptr);
    return n;
}

void free(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)((char *)ptr - HDR);
    b->free = 1;
    if (b->next && b->next->free) coalesce(b);
    if (b->prev && b->prev->free) coalesce(b->prev);
}

int atoi(const char *s) {
    return (int)atol(s);
}

long atol(const char *s) {
    long v = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

void exit(int code) {
    _exit(code);
    for (;;) {}
}

void abort(void) {
    _exit(134);
    for (;;) {}
}
