#include "stdlib.h"
#include "string.h"
#include "unistd.h"

/* --------------------------------------------------------------------------
 * Minimal heap allocator.
 *
 * A bump allocator backed by brk(2).  free() returns the most recent
 * allocation when possible; otherwise it is a no-op.  This is intentionally
 * simple - it is plenty for the shell and the small /bin programs, which are
 * short-lived and allocate modestly.
 * -------------------------------------------------------------------------- */

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static unsigned long heap_base = 0;   /* start of the heap (brk at startup) */
static unsigned long heap_end  = 0;   /* current brk */
static unsigned long heap_ptr  = 0;   /* next free byte */
static unsigned long last_alloc = 0;  /* start of the most recent allocation */

static int heap_init(void) {
    if (heap_base) return 0;
    unsigned long b = (unsigned long)sbrk_set(0);
    if ((long)b <= 0) return -1;
    heap_base = heap_ptr = heap_end = b;
    return 0;
}

void *malloc(size_t size) {
    if (heap_init() < 0) return NULL;
    if (size == 0) size = 1;
    size = ALIGN_UP(size, 16);

    unsigned long ret = heap_ptr;
    unsigned long need = ret + size;
    if (need > heap_end) {
        /* Grow the break in 64 KiB chunks to amortize syscalls. */
        unsigned long new_end = ALIGN_UP(need, 64 * 1024);
        unsigned long got = (unsigned long)sbrk_set(new_end);
        if (got < new_end) return NULL;
        heap_end = new_end;
    }
    heap_ptr = need;
    last_alloc = ret;
    return (void *)ret;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    /* We do not track allocation sizes; copy a conservative amount.  For the
     * heap's bump layout, [ptr, heap_ptr) is an upper bound of the old size. */
    void *n = malloc(size);
    if (!n) return NULL;
    size_t old = (size_t)(heap_ptr - (unsigned long)ptr);
    memcpy(n, ptr, old < size ? old : size);
    return n;
}

void free(void *ptr) {
    if (ptr && (unsigned long)ptr == last_alloc) {
        heap_ptr = last_alloc;   /* reclaim the most recent allocation */
        last_alloc = 0;
    }
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
