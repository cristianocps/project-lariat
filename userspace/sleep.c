/* sleep - suspend execution for N seconds. */

#include "libc/unistd.h"
#include "libc/errno.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"
#include "libc/time.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("usage: sleep SECONDS\n", STDERR_FILENO);
        return 1;
    }
    int secs = atoi(argv[1]);
    if (secs < 0) secs = 0;
    struct timespec req = { (int64_t)secs, 0 }, rem;
    /* Resume the remaining time if interrupted (e.g. SIGCONT after Ctrl-Z). */
    while (nanosleep(&req, &rem) < 0 && errno == EINTR)
        req = rem;
    return 0;
}
