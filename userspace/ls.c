/* ls - list directory contents (uses getdents64). */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"
#include "libc/string.h"
#include "libc/stdio.h"

/* Make `in` absolute relative to the cwd, writing into `out`. */
static void abspath(const char *in, char *out, size_t n) {
    if (in[0] == '/') {
        strncpy(out, in, n - 1);
        out[n - 1] = '\0';
        return;
    }
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");
    if (strcmp(cwd, "/") == 0) snprintf(out, n, "/%s", in);
    else                        snprintf(out, n, "%s/%s", cwd, in);
}

static int list_dir(const char *path) {
    char abs[256];
    abspath(path, abs, sizeof(abs));

    int fd = open(abs, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fprintf(STDERR_FILENO, "ls: cannot open '%s': error %d\n", path, errno);
        return 1;
    }

    char buf[2048];
    for (;;) {
        long n = getdents64(fd, buf, sizeof(buf));
        if (n < 0) {
            fprintf(STDERR_FILENO, "ls: read error %d\n", errno);
            close(fd);
            return 1;
        }
        if (n == 0) break;
        long off = 0;
        while (off < n) {
            struct dirent64 *d = (struct dirent64 *)(buf + off);
            if (strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0) {
                fputs(d->d_name, STDOUT_FILENO);
                if (d->d_type == DT_DIR) fputs("/", STDOUT_FILENO);
                fputs("\n", STDOUT_FILENO);
            }
            off += d->d_reclen;
        }
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    int rc = 0;
    if (argc < 2) {
        rc = list_dir(".");
    } else {
        for (int i = 1; i < argc; i++) {
            if (argc > 2) printf("%s:\n", argv[i]);
            rc |= list_dir(argv[i]);
        }
    }
    return rc;
}
