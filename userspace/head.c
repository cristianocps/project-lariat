/* head - print the first N lines (default 10). */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"

static int head_fd(int fd, int n) {
    freader fr;
    freader_init(&fr, fd);
    char line[2048];
    int count = 0;
    while (count < n && freader_getline(&fr, line, sizeof(line)) >= 0) {
        puts(line);
        count++;
    }
    return 0;
}

int main(int argc, char **argv) {
    int n = 10;
    int first_file = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n = atoi(argv[++i]);
            continue;
        }
        first_file = i;
        break;
    }
    if (first_file == 0) return head_fd(STDIN_FILENO, n);
    int rc = 0;
    for (int i = first_file; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(STDERR_FILENO, "head: %s: error %d\n", argv[i], errno); rc = 1; continue; }
        if ((argc - first_file) > 1) printf("==> %s <==\n", argv[i]);
        head_fd(fd, n);
        close(fd);
    }
    return rc;
}
