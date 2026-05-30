/* cp - copy a file. */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"
#include "libc/stdio.h"

static int copy(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(STDERR_FILENO, "cp: %s: error %d\n", src, errno);
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        fprintf(STDERR_FILENO, "cp: %s: error %d\n", dst, errno);
        close(in);
        return 1;
    }
    char buf[4096];
    int n, rc = 0;
    while ((n = (int)read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
            fprintf(STDERR_FILENO, "cp: write error %d\n", errno);
            rc = 1;
            break;
        }
    }
    close(in);
    close(out);
    return rc;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("usage: cp SRC DST\n", STDERR_FILENO);
        return 1;
    }
    return copy(argv[1], argv[2]);
}
