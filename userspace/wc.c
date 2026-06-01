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

/* Print the selected counts (lines, words, bytes - in that fixed order) for one
 * input, followed by an optional label.  When no -l/-w/-c flag is given, all
 * three are shown, matching POSIX wc. */
static void report(long l, long w, long b, int wl, int ww, int wb,
                   const char *label) {
    int first = 1;
    if (wl) { printf(first ? "%ld" : " %ld", l); first = 0; }
    if (ww) { printf(first ? "%ld" : " %ld", w); first = 0; }
    if (wb) { printf(first ? "%ld" : " %ld", b); first = 0; }
    if (label) printf(" %s", label);
    printf("\n");
}

int main(int argc, char **argv) {
    int wl = 0, ww = 0, wb = 0;          /* which counts the user asked for */
    long tl = 0, tw = 0, tb = 0;
    int files = 0, rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *c = argv[i] + 1; *c; c++) {
                if (*c == 'l') wl = 1;
                else if (*c == 'w') ww = 1;
                else if (*c == 'c' || *c == 'm') wb = 1;
            }
        }
    }
    if (!wl && !ww && !wb) { wl = ww = wb = 1; }  /* default: all three */

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;  /* a flag, already parsed */
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
        report(l, w, b, wl, ww, wb, argv[i]);
        tl += l; tw += w; tb += b;
    }
    if (files == 0) {
        long l, w, b;
        count_fd(STDIN_FILENO, &l, &w, &b);
        report(l, w, b, wl, ww, wb, NULL);
    } else if (files > 1) {
        report(tl, tw, tb, wl, ww, wb, "total");
    }
    return rc;
}
