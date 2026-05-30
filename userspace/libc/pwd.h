#ifndef LIBC_PWD_H
#define LIBC_PWD_H

#include "libc/unistd.h"

struct passwd {
    char  *pw_name;
    char  *pw_passwd;   /* always "x"; real hash lives in /etc/shadow */
    uid_t  pw_uid;
    gid_t  pw_gid;
    char  *pw_gecos;
    char  *pw_dir;
    char  *pw_shell;
};

/* Parse /etc/passwd.  Both return a pointer to a static struct (overwritten by
 * the next call) or NULL if no matching entry exists. */
struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);

/* Fetch the stored crypt-lite hash for `name` from /etc/shadow into `out`.
 * Returns 0 on success, -1 if not found / unreadable. */
int shadow_get(const char *name, char *out, size_t outsz);

/* Replace (or add) `name`'s hash in /etc/shadow.  Returns 0 on success. */
int shadow_set(const char *name, const char *hash);

/* crypt-lite wrapper: hash `key` using the salt embedded in `setting`. */
char *crypt(const char *key, const char *setting);

#endif /* LIBC_PWD_H */
