/* Minimal environment handling: getenv / setenv / unsetenv / putenv.
 *
 * `environ` is published by crt0 (points at the initial envp on the stack).
 * On the first modification we copy it into a malloc'd, growable vector so we
 * can add/replace entries. */

#include "stdlib.h"
#include "string.h"

char **environ = 0;

static char **env_vec = 0;     /* our owned copy (NULL until first write) */
static int    env_len = 0;     /* number of entries (excluding NULL) */
static int    env_cap = 0;

static int name_matches(const char *entry, const char *name, size_t nlen) {
    return strncmp(entry, name, nlen) == 0 && entry[nlen] == '=';
}

char *getenv(const char *name) {
    if (!environ || !name) return 0;
    size_t nlen = strlen(name);
    for (char **e = environ; *e; e++) {
        if (name_matches(*e, name, nlen)) return *e + nlen + 1;
    }
    return 0;
}

/* Switch from the read-only initial environ to our owned, growable copy. */
static int env_take_ownership(void) {
    if (env_vec) return 0;
    int n = 0;
    if (environ) while (environ[n]) n++;
    env_cap = n + 8;
    env_vec = (char **)malloc((size_t)env_cap * sizeof(char *));
    if (!env_vec) return -1;
    for (int i = 0; i < n; i++) env_vec[i] = environ[i];
    env_vec[n] = 0;
    env_len = n;
    environ = env_vec;
    return 0;
}

static int env_grow(void) {
    int ncap = env_cap * 2;
    char **nv = (char **)malloc((size_t)ncap * sizeof(char *));
    if (!nv) return -1;
    for (int i = 0; i <= env_len; i++) nv[i] = env_vec[i];
    free(env_vec);
    env_vec = nv;
    env_cap = ncap;
    environ = env_vec;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) return -1;
    if (env_take_ownership() < 0) return -1;

    size_t nlen = strlen(name);
    size_t vlen = strlen(value ? value : "");
    char *entry = (char *)malloc(nlen + vlen + 2);
    if (!entry) return -1;
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value ? value : "", vlen);
    entry[nlen + 1 + vlen] = '\0';

    for (int i = 0; i < env_len; i++) {
        if (name_matches(env_vec[i], name, nlen)) {
            if (!overwrite) { free(entry); return 0; }
            env_vec[i] = entry;   /* leak the old one (no refcount) */
            return 0;
        }
    }
    if (env_len + 1 >= env_cap && env_grow() < 0) { free(entry); return -1; }
    env_vec[env_len++] = entry;
    env_vec[env_len] = 0;
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) return -1;
    if (!environ) return 0;
    if (env_take_ownership() < 0) return -1;
    size_t nlen = strlen(name);
    for (int i = 0; i < env_len; i++) {
        if (name_matches(env_vec[i], name, nlen)) {
            for (int j = i; j < env_len; j++) env_vec[j] = env_vec[j + 1];
            env_len--;
            i--;
        }
    }
    return 0;
}

int putenv(char *string) {
    if (!string) return -1;
    char *eq = strchr(string, '=');
    if (!eq) return unsetenv(string);
    if (env_take_ownership() < 0) return -1;
    size_t nlen = (size_t)(eq - string);
    for (int i = 0; i < env_len; i++) {
        if (name_matches(env_vec[i], string, nlen)) { env_vec[i] = string; return 0; }
    }
    if (env_len + 1 >= env_cap && env_grow() < 0) return -1;
    env_vec[env_len++] = string;
    env_vec[env_len] = 0;
    return 0;
}
