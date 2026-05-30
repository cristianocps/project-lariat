/* ps - list processes (via the Lariat-specific SYS_LARIAT_PS syscall). */

#include "libc/unistd.h"
#include "libc/stdio.h"

/* Must match struct proc_info in include/sched.h. */
struct proc_info {
    int  pid;
    int  ppid;
    int  pgid;
    char state;
    char name[32];
};

#define MAXPROC 64

int main(void) {
    struct proc_info procs[MAXPROC];
    long n = syscall2(SYS_LARIAT_PS, (long)procs, MAXPROC);
    if (n < 0) {
        fputs("ps: not supported\n", STDERR_FILENO);
        return 1;
    }
    printf("  PID  PPID  PGID S CMD\n");
    for (long i = 0; i < n; i++) {
        printf("%d %d %d %c %s\n", procs[i].pid, procs[i].ppid,
               procs[i].pgid, procs[i].state, procs[i].name);
    }
    return 0;
}
