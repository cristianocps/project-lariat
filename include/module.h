#ifndef MODULE_H
#define MODULE_H

#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Module / Loadable Kernel Module (LKM) framework
 *
 * For now: statically linked modules register via macros.
 * Future: load ELF .ko files at runtime, resolve symbols, call init/exit.
 * -------------------------------------------------------------------------- */

#define MODULE_NAME_MAX 64

typedef struct module {
    char        name[MODULE_NAME_MAX];
    char        version[32];

    /* ELF image (when loaded dynamically) */
    void       *image;
    size_t      image_size;

    /* Symbol table (exported symbols) */
    void      **symtab;
    const char **strtab;
    size_t      sym_count;

    /* Init / exit hooks */
    int  (*init)(void);
    void (*exit)(void);

    /* Reference count */
    uint32_t    ref_count;

    struct module *next;
} module_t;

/* --------------------------------------------------------------------------
 * Static module declaration (compile-time)
 * -------------------------------------------------------------------------- */
#define MODULE_STATIC(name_, version_, init_fn, exit_fn) \
    static module_t __module_##name_ = { \
        .name = #name_, \
        .version = version_, \
        .init = init_fn, \
        .exit = exit_fn, \
        .ref_count = 0, \
    }; \
    static void __attribute__((constructor)) __module_register_##name_(void) { \
        module_register(&__module_##name_); \
    }

/* --------------------------------------------------------------------------
 * Module API
 * -------------------------------------------------------------------------- */
void module_init(void);

int  module_register(module_t *mod);
void module_unregister(module_t *mod);

module_t *module_find(const char *name);

/* Symbol export / import */
int   module_export(module_t *mod, const char *name, void *addr);
void *module_resolve(const char *name);

/* Future: dynamic loading */
int module_load(const char *path);   /* Load ELF .ko */
int module_unload(const char *name); /* Unload by name */

/* Iteration */
void module_foreach(void (*cb)(module_t *mod, void *ctx), void *ctx);

#endif
