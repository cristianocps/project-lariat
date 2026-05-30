/* clear - clear the terminal via ANSI escapes. */

#include "libc/unistd.h"

int main(void) {
    const char *seq = "\033[2J\033[H";
    write(STDOUT_FILENO, seq, 7);
    return 0;
}
