/* Userspace init program - tests POSIX syscalls */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"

static size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void puts1(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }

/* wait status encodes the exit code in bits 8-15 (like WEXITSTATUS). */
#define WEXITSTATUS(st) (((st) >> 8) & 0xff)

static void put_int(long v) {
    char s[24];
    int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    do { s[i++] = '0' + (v % 10); v /= 10; } while (v > 0);
    if (neg) s[i++] = '-';
    char out[24];
    int j = 0;
    while (i > 0) out[j++] = s[--i];
    write(STDOUT_FILENO, out, j);
}

/* --------------------------------------------------------------------------
 * Stress tests: hammer fork/exit, brk, mmap, and pipe IPC to surface races,
 * leaks, and page-table teardown bugs.  Each prints a one-line PASS/FAIL.
 * -------------------------------------------------------------------------- */
static void stress_tests(void) {
    puts1("\n[stress] begin\n");

    /* 1. Fork storm: many short-lived children that each touch heap, all reaped.
     *    Exercises page-table clone + teardown, PMM churn, and zombie reaping. */
    const int N = 64;
    int reaped = 0, status_ok = 1;
    for (int i = 0; i < N; i++) {
        int pid = fork();
        if (pid < 0) { puts1("[stress] fork failed at "); put_int(i); puts1("\n"); break; }
        if (pid == 0) {
            unsigned long base = (unsigned long)sbrk_set(0);
            sbrk_set(base + 8192);
            volatile unsigned char *p = (volatile unsigned char *)base;
            p[0] = (unsigned char)i; p[4095] = (unsigned char)i; p[8191] = (unsigned char)i;
            if (p[0] != (unsigned char)i || p[8191] != (unsigned char)i) _exit(1);
            _exit(0);
        }
        int st = -1;
        if (waitpid(pid, &st, 0) == pid) { reaped++; if (WEXITSTATUS(st) != 0) status_ok = 0; }
    }
    puts1("[stress] fork storm: reaped "); put_int(reaped); puts1("/"); put_int(N);
    puts1(status_ok ? " (status ok)\n" : " (BAD STATUS)\n");

    /* 2. brk churn: grow/shrink the heap repeatedly, verifying writes survive. */
    unsigned long b0 = (unsigned long)sbrk_set(0);
    int brk_ok = 1;
    for (int it = 0; it < 32; it++) {
        sbrk_set(b0 + 64 * 1024);
        volatile unsigned char *h = (volatile unsigned char *)b0;
        for (int k = 0; k < 64 * 1024; k += 4096) h[k] = (unsigned char)(k + it);
        for (int k = 0; k < 64 * 1024; k += 4096)
            if (h[k] != (unsigned char)(k + it)) brk_ok = 0;
        sbrk_set(b0);
    }
    puts1(brk_ok ? "[stress] brk churn: ok\n" : "[stress] brk churn: FAILED\n");

    /* 3. Anonymous mmap: map, write, read back. */
    int mmap_ok = 1;
    for (int it = 0; it < 16; it++) {
        unsigned char *m = (unsigned char *)mmap(0, 16 * 1024,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if ((long)m <= 0) { mmap_ok = 0; break; }
        for (int k = 0; k < 16 * 1024; k += 4096) m[k] = (unsigned char)(it + k);
        for (int k = 0; k < 16 * 1024; k += 4096)
            if (m[k] != (unsigned char)(it + k)) mmap_ok = 0;
    }
    puts1(mmap_ok ? "[stress] mmap anon: ok\n" : "[stress] mmap anon: FAILED\n");

    /* 4. Pipe IPC round-trips through fork. */
    int ipc_ok = 1;
    for (int it = 0; it < 8; it++) {
        int fds[2];
        if (pipe(fds) != 0) { ipc_ok = 0; break; }
        int p = fork();
        if (p < 0) { ipc_ok = 0; break; }
        if (p == 0) {
            close(fds[1]);
            unsigned char b = 0;
            int n = read(fds[0], &b, 1);
            close(fds[0]);
            _exit(n == 1 ? (int)b : 99);
        }
        close(fds[0]);
        unsigned char tok = (unsigned char)(it + 1);
        write(fds[1], &tok, 1);
        close(fds[1]);
        int st = -1;
        if (waitpid(p, &st, 0) != p || WEXITSTATUS(st) != tok) ipc_ok = 0;
    }
    puts1(ipc_ok ? "[stress] pipe IPC: ok\n" : "[stress] pipe IPC: FAILED\n");

    puts1("[stress] done\n");
}

int main(void) {
    puts1("Hello from userspace!\n");

    /* Process identity */
    puts1("pid="); put_int(getpid());
    puts1(" ppid="); put_int(getppid()); puts1("\n");

    /* fork + waitpid */
    int pid = fork();
    if (pid < 0) {
        puts1("fork failed\n");
    } else if (pid == 0) {
        puts1("I am the child, my pid="); put_int(getpid());
        puts1(" ppid="); put_int(getppid()); puts1("\n");
        _exit(42);
    } else {
        puts1("I am the parent\n");
        int status = 0;
        int ret = waitpid(pid, &status, 0);
        if (ret == pid) {
            puts1("Child exit status (raw): "); put_int(status); puts1("\n");
        } else {
            puts1("waitpid failed\n");
        }
    }

    /* pipe + fork IPC */
    int fds[2];
    if (pipe(fds) == 0) {
        int p = fork();
        if (p == 0) {
            close(fds[1]);
            char b[32];
            int n = read(fds[0], b, sizeof(b));
            puts1("pipe child read: ");
            if (n > 0) write(STDOUT_FILENO, b, n);
            close(fds[0]);
            _exit(0);
        } else {
            close(fds[0]);
            const char *m = "ping-via-pipe\n";
            write(fds[1], m, strlen(m));
            close(fds[1]);
            waitpid(p, 0, 0);
        }
    } else {
        puts1("pipe failed\n");
    }

    /* dup2: redirect fd 7 to stdout */
    if (dup2(STDOUT_FILENO, 7) == 7) {
        const char *m = "written via dup2 fd 7\n";
        write(7, m, strlen(m));
        close(7);
    }

    /* mkdir */
    if (mkdir("/tmp", 0755) == 0) {
        puts1("mkdir /tmp ok\n");
    } else {
        puts1("mkdir /tmp failed errno="); put_int(errno); puts1("\n");
    }

    /* open + read from ramfs */
    int fd = open("/hello.txt", O_RDONLY);
    if (fd < 0) {
        puts1("Failed to open /hello.txt\n");
        return 1;
    }
    char buf[64];
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        puts1("Contents: ");
        write(STDOUT_FILENO, buf, n);
    } else {
        puts1("Failed to read /hello.txt\n");
    }
    close(fd);

    stress_tests();

    puts1("Userspace init done.\n");

    /* PID 1 stays alive as root and runs a getty/login loop: fork a child that
     * execs /bin/login on the console, wait for the session to end, respawn.
     * login authenticates against /etc/passwd + /etc/shadow and drops to the
     * target user before exec'ing their shell. */
    for (;;) {
        int pid = fork();
        if (pid < 0) {
            puts1("init: fork failed\n");
            continue;
        }
        if (pid == 0) {
            char *login_argv[] = { "/bin/login", 0 };
            execve("/bin/login", login_argv, 0);
            puts1("init: failed to exec /bin/login\n");
            _exit(127);
        }
        /* Reap the login session (and any orphans adopted by PID 1). */
        int st;
        while (waitpid(-1, &st, 0) != pid) { /* keep reaping */ }
    }
    return 0;
}
