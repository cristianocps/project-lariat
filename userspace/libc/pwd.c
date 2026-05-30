#include "libc/pwd.h"
#include "libc/fcntl.h"
#include "libc/string.h"
#include "libc/stdio.h"
#include <crypt_lite.h>

/* Read an entire small text file into buf (NUL-terminated).  Returns length
 * or -1 on error. */
static int slurp(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t off = 0;
    for (;;) {
        if (off + 1 >= bufsz) break;
        long n = read(fd, buf + off, bufsz - 1 - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);
    buf[off] = '\0';
    return (int)off;
}

/* Split `line` in place on ':' into up to maxf fields.  Returns field count. */
static int splitc(char *line, char sep, char **fields, int maxf) {
    int n = 0;
    fields[n++] = line;
    for (char *p = line; *p && n < maxf; p++) {
        if (*p == sep) { *p = '\0'; fields[n++] = p + 1; }
    }
    return n;
}

static unsigned to_uint(const char *s) {
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

static struct passwd g_pw;
static char g_pwline[256];

static struct passwd *parse_match(const char *want_name, int want_uid_valid,
                                  uid_t want_uid) {
    static char filebuf[2048];
    if (slurp("/etc/passwd", filebuf, sizeof(filebuf)) < 0) return 0;

    char *save = 0;
    for (char *line = strtok_r(filebuf, "\n", &save); line;
         line = strtok_r(0, "\n", &save)) {
        if (line[0] == '\0' || line[0] == '#') continue;
        /* Copy into the persistent line buffer (struct fields point into it). */
        size_t l = strlen(line);
        if (l >= sizeof(g_pwline)) continue;
        memcpy(g_pwline, line, l + 1);

        char *f[7];
        int nf = splitc(g_pwline, ':', f, 7);
        if (nf < 7) continue;

        uid_t uid = (uid_t)to_uint(f[2]);
        int match = want_name ? (strcmp(f[0], want_name) == 0)
                              : (want_uid_valid && uid == want_uid);
        if (!match) continue;

        g_pw.pw_name   = f[0];
        g_pw.pw_passwd = f[1];
        g_pw.pw_uid    = uid;
        g_pw.pw_gid    = (gid_t)to_uint(f[3]);
        g_pw.pw_gecos  = f[4];
        g_pw.pw_dir    = f[5];
        g_pw.pw_shell  = f[6];
        return &g_pw;
    }
    return 0;
}

struct passwd *getpwnam(const char *name) {
    return parse_match(name, 0, 0);
}

struct passwd *getpwuid(uid_t uid) {
    return parse_match(0, 1, uid);
}

int shadow_get(const char *name, char *out, size_t outsz) {
    static char filebuf[2048];
    if (slurp("/etc/shadow", filebuf, sizeof(filebuf)) < 0) return -1;
    char *save = 0;
    for (char *line = strtok_r(filebuf, "\n", &save); line;
         line = strtok_r(0, "\n", &save)) {
        if (line[0] == '\0') continue;
        char tmp[256];
        size_t l = strlen(line);
        if (l >= sizeof(tmp)) continue;
        memcpy(tmp, line, l + 1);
        char *f[3];
        int nf = splitc(tmp, ':', f, 3);
        if (nf < 2) continue;
        if (strcmp(f[0], name) == 0) {
            size_t hl = strlen(f[1]);
            if (hl + 1 > outsz) hl = outsz - 1;
            memcpy(out, f[1], hl);
            out[hl] = '\0';
            return 0;
        }
    }
    return -1;
}

int shadow_set(const char *name, const char *hash) {
    static char filebuf[2048];
    int len = slurp("/etc/shadow", filebuf, sizeof(filebuf));
    if (len < 0) return -1;

    static char outbuf[2048];
    size_t o = 0;
    int replaced = 0;
    char *save = 0;
    for (char *line = strtok_r(filebuf, "\n", &save); line;
         line = strtok_r(0, "\n", &save)) {
        if (line[0] == '\0') continue;
        char tmp[256];
        size_t l = strlen(line);
        if (l >= sizeof(tmp)) continue;
        memcpy(tmp, line, l + 1);
        char *colon = strchr(tmp, ':');
        if (colon) *colon = '\0';
        const char *user = tmp;
        int n;
        if (strcmp(user, name) == 0) {
            n = snprintf(outbuf + o, sizeof(outbuf) - o, "%s:%s:\n", name, hash);
            replaced = 1;
        } else {
            n = snprintf(outbuf + o, sizeof(outbuf) - o, "%s\n", line);
        }
        if (n > 0) o += (size_t)n;
    }
    if (!replaced) {
        int n = snprintf(outbuf + o, sizeof(outbuf) - o, "%s:%s:\n", name, hash);
        if (n > 0) o += (size_t)n;
    }

    int fd = open("/etc/shadow", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    long w = write(fd, outbuf, o);
    close(fd);
    return (w == (long)o) ? 0 : -1;
}

char *crypt(const char *key, const char *setting) {
    static char buf[64];
    crypt_lite(key, setting, buf, sizeof(buf));
    return buf;
}
