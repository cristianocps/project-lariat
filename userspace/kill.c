/* kill - send a signal to a process (default TERM). */

#include "libc/unistd.h"
#include "libc/errno.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/stdio.h"

static int signame(const char *s) {
    if (strcmp(s, "TERM") == 0) return 15;
    if (strcmp(s, "KILL") == 0) return 9;
    if (strcmp(s, "INT")  == 0) return 2;
    if (strcmp(s, "CONT") == 0) return 18;
    if (strcmp(s, "STOP") == 0) return 19;
    if (strcmp(s, "TSTP") == 0) return 20;
    return atoi(s);
}

int main(int argc, char **argv) {
    int sig = 15;
    int i = 1;
    if (i < argc && argv[i][0] == '-') {
        sig = signame(argv[i] + 1);
        i++;
    }
    if (i >= argc) {
        fputs("usage: kill [-SIG] PID...\n", STDERR_FILENO);
        return 1;
    }
    int rc = 0;
    for (; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (kill(pid, sig) < 0) {
            fprintf(STDERR_FILENO, "kill: (%d) error %d\n", pid, errno);
            rc = 1;
        }
    }
    return rc;
}
