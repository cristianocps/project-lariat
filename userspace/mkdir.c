/* mkdir - create directories. */

#include "libc/unistd.h"
#include "libc/errno.h"
#include "libc/stdio.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("usage: mkdir DIR...\n", STDERR_FILENO);
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            fprintf(STDERR_FILENO, "mkdir: %s: error %d\n", argv[i], errno);
            rc = 1;
        }
    }
    return rc;
}
