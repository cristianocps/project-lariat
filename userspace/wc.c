/* wc - count lines, words, and bytes. */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"
#include "libc/stdio.h"

static void count_fd(int fd, long *lines, long *words, long *bytes) {
    freader fr;
    freader_init(&fr, fd);
    int c, inword = 0;
    long l = 0, w = 0, b = 0;
    while ((c = freader_getc(&fr)) >= 0) {
        b++;
        if (c == '\n') l++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            inword = 0;
        } else if (!inword) {
            inword = 1;
            w++;
        }
    }
    *lines = l; *words = w; *bytes = b;
}

int main(int argc, char **argv) {
    long tl = 0, tw = 0, tb = 0;
    int files = 0, rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;  /* ignore flags */
        files++;
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(STDERR_FILENO, "wc: %s: error %d\n", argv[i], errno);
            rc = 1;
            continue;
        }
        long l, w, b;
        count_fd(fd, &l, &w, &b);
        close(fd);
        printf("%ld %ld %ld %s\n", l, w, b, argv[i]);
        tl += l; tw += w; tb += b;
    }
    if (files == 0) {
        long l, w, b;
        count_fd(STDIN_FILENO, &l, &w, &b);
        printf("%ld %ld %ld\n", l, w, b);
    } else if (files > 1) {
        printf("%ld %ld %ld total\n", tl, tw, tb);
    }
    return rc;
}
