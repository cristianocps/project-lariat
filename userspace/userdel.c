/* userdel - delete a local account.
 *
 * Removes the user's entry from /etc/passwd, /etc/shadow, and /etc/group (by
 * matching the first ':'-separated field), then mirrors the databases to the
 * persistent store (/var/etc).  The home directory is left in place.  Must be
 * run as root.
 *
 * Usage: userdel NAME
 */

#include "libc/unistd.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/fcntl.h"
#include "libc/pwd.h"

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

/* Drop every line of /etc/<dbname> whose first ':'-field equals `name`.
 * Returns the number of lines removed, or -1 on I/O error. */
static int remove_user_lines(const char *dbname, const char *name) {
    char path[64];
    snprintf(path, sizeof(path), "/etc/%s", dbname);

    static char in[DB_MAX];
    int len = slurp(path, in, sizeof(in));
    if (len < 0) return -1;

    static char out[DB_MAX];
    size_t o = 0;
    int removed = 0;
    char *save = 0;
    for (char *line = strtok_r(in, "\n", &save); line;
         line = strtok_r(0, "\n", &save)) {
        if (line[0] == '\0') continue;

        char first[64];
        size_t i = 0;
        for (; line[i] && line[i] != ':' && i < sizeof(first) - 1; i++)
            first[i] = line[i];
        first[i] = '\0';

        if (strcmp(first, name) == 0) { removed++; continue; }

        int n = snprintf(out + o, sizeof(out) - o, "%s\n", line);
        if (n > 0) o += (size_t)n;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    write(fd, out, o);
    close(fd);
    return removed;
}

int main(int argc, char **argv) {
    if (getuid() != 0) {
        fputs("userdel: must be root\n", STDERR_FILENO);
        return 1;
    }
    if (argc < 2 || argv[1][0] == '\0') {
        fputs("usage: userdel NAME\n", STDERR_FILENO);
        return 1;
    }
    const char *name = argv[1];

    if (!getpwnam(name)) {
        fprintf(STDERR_FILENO, "userdel: user %s does not exist\n", name);
        return 1;
    }

    int r = remove_user_lines("passwd", name);
    if (r <= 0) {
        fprintf(STDERR_FILENO, "userdel: failed to remove %s from /etc/passwd\n", name);
        return 1;
    }
    remove_user_lines("shadow", name);
    remove_user_lines("group", name);

    etc_sync("passwd");
    etc_sync("shadow");
    etc_sync("group");

    printf("userdel: removed %s\n", name);
    return 0;
}
