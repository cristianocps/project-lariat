/* cat - concatenate files to stdout (or echo stdin if no file given). */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"
#include "libc/string.h"
#include "libc/stdio.h"

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

static int cat_fd(int fd) {
    char buf[1024];
    for (;;) {
        int n = read(fd, buf, sizeof(buf));
        if (n < 0) return 1;
        if (n == 0) break;
        write(STDOUT_FILENO, buf, n);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2)
        return cat_fd(STDIN_FILENO);

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char abs[256];
        abspath(argv[i], abs, sizeof(abs));
        int fd = open(abs, O_RDONLY);
        if (fd < 0) {
            fprintf(STDERR_FILENO, "cat: %s: error %d\n", argv[i], errno);
            rc = 1;
            continue;
        }
        rc |= cat_fd(fd);
        close(fd);
    }
    return rc;
}
