#include "module.h"
#include "kapi.h"

static module_t *module_list = NULL;
static spinlock_t module_lock = SPINLOCK_INIT;

void module_init(void) {
    module_list = NULL;
    module_lock.locked = 0;
}

int module_register(module_t *mod) {
    if (!mod || !mod->name[0]) return -1;

    uint64_t flags = spin_lock_irqsave(&module_lock);
    mod->next = module_list;
    module_list = mod;
    spin_unlock_irqrestore(&module_lock, flags);

    KAPI_INFO("registered module: %s v%s\n", mod->name, mod->version);

    if (mod->init) {
        int rc = mod->init();
        if (rc < 0) {
            KAPI_ERR("module '%s' init failed: %d\n", mod->name, rc);
            module_unregister(mod);
            return rc;
        }
    }

    return 0;
}

void module_unregister(module_t *mod) {
    if (!mod) return;

    if (mod->exit) {
        mod->exit();
    }

    uint64_t flags = spin_lock_irqsave(&module_lock);
    module_t **pp = &module_list;
    while (*pp && *pp != mod) {
        pp = &(*pp)->next;
    }
    if (*pp == mod) {
        *pp = mod->next;
    }
    spin_unlock_irqrestore(&module_lock, flags);
}

module_t *module_find(const char *name) {
    uint64_t flags = spin_lock_irqsave(&module_lock);
    for (module_t *mod = module_list; mod; mod = mod->next) {
        const char *a = mod->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) {
            spin_unlock_irqrestore(&module_lock, flags);
            return mod;
        }
    }
    spin_unlock_irqrestore(&module_lock, flags);
    return NULL;
}

int module_export(module_t *mod, const char *name, void *addr) {
    (void)mod;
    (void)name;
    (void)addr;
    /* TODO: symbol table management */
    return 0;
}

void *module_resolve(const char *name) {
    (void)name;
    /* TODO: lookup in exported symbol tables */
    return NULL;
}

int module_load(const char *path) {
    (void)path;
    KAPI_ERR("dynamic module loading not yet implemented\n");
    return -1;
}

int module_unload(const char *name) {
    module_t *mod = module_find(name);
    if (!mod) return -1;
    module_unregister(mod);
    return 0;
}

void module_foreach(void (*cb)(module_t *mod, void *ctx), void *ctx) {
    uint64_t flags = spin_lock_irqsave(&module_lock);
    for (module_t *mod = module_list; mod; mod = mod->next) {
        cb(mod, ctx);
    }
    spin_unlock_irqrestore(&module_lock, flags);
}
