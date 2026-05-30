/* tail - print the last N lines (default 10). */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"

#define MAXKEEP 64
#define LINEW   512

static int tail_fd(int fd, int n) {
    if (n > MAXKEEP) n = MAXKEEP;
    if (n < 1) n = 1;
    static char ring[MAXKEEP][LINEW];
    int count = 0, head = 0;
    freader fr;
    freader_init(&fr, fd);
    char line[2048];
    while (freader_getline(&fr, line, sizeof(line)) >= 0) {
        strncpy(ring[head], line, LINEW - 1);
        ring[head][LINEW - 1] = '\0';
        head = (head + 1) % n;
        if (count < n) count++;
    }
    int start = (count < n) ? 0 : head;
    for (int i = 0; i < count; i++) {
        puts(ring[(start + i) % n]);
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
    if (first_file == 0) return tail_fd(STDIN_FILENO, n);
    int rc = 0;
    for (int i = first_file; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(STDERR_FILENO, "tail: %s: error %d\n", argv[i], errno); rc = 1; continue; }
        if ((argc - first_file) > 1) printf("==> %s <==\n", argv[i]);
        tail_fd(fd, n);
        close(fd);
    }
    return rc;
}
