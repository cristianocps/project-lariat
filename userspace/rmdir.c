/* rmdir - remove empty directories. */

#include "libc/unistd.h"
#include "libc/errno.h"
#include "libc/stdio.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("usage: rmdir DIR...\n", STDERR_FILENO);
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            fprintf(STDERR_FILENO, "rmdir: %s: error %d\n", argv[i], errno);
            rc = 1;
        }
    }
    return rc;
}
