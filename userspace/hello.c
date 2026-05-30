/* hello - a tiny demo program proving fork()+execve() of a separate binary. */

#include "libc/unistd.h"
#include "libc/stdio.h"

int main(int argc, char **argv) {
    printf("Hello from /bin/hello!  pid=%d ppid=%d argc=%d\n",
           getpid(), getppid(), argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);
    return 0;
}
