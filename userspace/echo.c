/* echo - write arguments to stdout, separated by spaces. */

#include "libc/unistd.h"
#include "libc/string.h"

int main(int argc, char **argv) {
    int start = 1;
    int newline = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        start = 2;
    }
    for (int i = start; i < argc; i++) {
        write(STDOUT_FILENO, argv[i], strlen(argv[i]));
        if (i + 1 < argc) write(STDOUT_FILENO, " ", 1);
    }
    if (newline) write(STDOUT_FILENO, "\n", 1);
    return 0;
}
