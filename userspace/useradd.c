/* useradd - create a local account.
 *
 * Appends an entry to /etc/passwd, a locked entry to /etc/shadow, and a
 * matching primary group to /etc/group, creates the home directory, then
 * mirrors all three databases to the persistent store (/var/etc) so the new
 * account survives a reboot.  Must be run as root.
 *
 * Usage: useradd [-u UID] [-g GID] [-d HOME] [-s SHELL] [-c GECOS] NAME
 */

#include "libc/unistd.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/fcntl.h"
#include "libc/pwd.h"
#include "libc/sys/stat.h"

#define DB_MAX 16384

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

/* Append `line` (which should already end with '\n') to /etc/<name>. */
static int append_line(const char *name, const char *line) {
    char path[64];
    snprintf(path, sizeof(path), "/etc/%s", name);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) return -1;
    size_t len = strlen(line);
    long w = write(fd, line, len);
    close(fd);
    return (w == (long)len) ? 0 : -1;
}

static unsigned to_uint(const char *s) {
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}

/* Highest uid in [1000,60000) currently present in /etc/passwd, or 999. */
static unsigned max_normal_uid(void) {
    static char buf[DB_MAX];
    unsigned best = 999;
    if (slurp("/etc/passwd", buf, sizeof(buf)) < 0) return best;
    char *save = 0;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(0, "\n", &save)) {
        if (line[0] == '\0' || line[0] == '#') continue;
        char *c1 = strchr(line, ':');
        if (!c1) continue;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        unsigned uid = to_uint(c2 + 1);
        if (uid >= 1000 && uid < 60000 && uid > best) best = uid;
    }
    return best;
}

int main(int argc, char **argv) {
    if (getuid() != 0) {
        fputs("useradd: must be root\n", STDERR_FILENO);
        return 1;
    }

    const char *name = 0, *home = 0, *shell = "/bin/sh", *gecos = "";
    long uid = -1, gid = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)      uid = (long)to_uint(argv[++i]);
        else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) gid = (long)to_uint(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) home = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) shell = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) gecos = argv[++i];
        else if (argv[i][0] != '-')                          name = argv[i];
        else { fprintf(STDERR_FILENO, "useradd: unknown option %s\n", argv[i]); return 1; }
    }

    if (!name || name[0] == '\0') {
        fputs("usage: useradd [-u UID] [-g GID] [-d HOME] [-s SHELL] [-c GECOS] NAME\n",
              STDERR_FILENO);
        return 1;
    }
    if (strchr(name, ':') || strchr(name, '/') || strchr(name, '\n')) {
        fputs("useradd: invalid user name\n", STDERR_FILENO);
        return 1;
    }
    if (getpwnam(name)) {
        fprintf(STDERR_FILENO, "useradd: user %s already exists\n", name);
        return 1;
    }

    if (uid < 0) uid = (long)(max_normal_uid() + 1);
    if (gid < 0) gid = uid;
    char homebuf[80];
    if (!home) { snprintf(homebuf, sizeof(homebuf), "/home/%s", name); home = homebuf; }

    char line[256];
    snprintf(line, sizeof(line), "%s:x:%ld:%ld:%s:%s:%s\n",
             name, uid, gid, gecos, home, shell);
    if (append_line("passwd", line) != 0) {
        fputs("useradd: failed to update /etc/passwd\n", STDERR_FILENO);
        return 1;
    }

    /* Locked password ("*") - set one later with passwd. */
    snprintf(line, sizeof(line), "%s:*:\n", name);
    append_line("shadow", line);

    /* Primary group mirrors the account. */
    snprintf(line, sizeof(line), "%s:x:%ld:\n", name, gid);
    append_line("group", line);

    mkdir("/home", 0755);   /* harmless if it already exists */
    if (mkdir(home, 0755) != 0) {
        fprintf(STDERR_FILENO, "useradd: warning: could not create %s\n", home);
    }

    etc_sync("passwd");
    etc_sync("shadow");
    etc_sync("group");

    printf("useradd: created %s (uid=%ld gid=%ld home=%s)\n", name, uid, gid, home);
    return 0;
}
