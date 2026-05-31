/* Userspace init program - tests POSIX syscalls */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"
#include "wsproto.h"
#include <crypt_lite.h>

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

static int has_prefix(const char *s, const char *p) {
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}
static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    for (size_t i = 0; i < lf; i++) if (s[ls - lf + i] != suf[i]) return 0;
    return 1;
}

/* --------------------------------------------------------------------------
 * Minimal service supervisor (Phase 4: "init toward services").
 *
 * PID 1 reads service definitions from /etc/services.d/<name>.conf, each a
 * key=value file with an `exec=` line and an optional `respawn=1`.  Services
 * are started at boot; respawn services are restarted when they exit.  The
 * console login session is itself a managed (respawn) service.
 * -------------------------------------------------------------------------- */
#define MAXSVC 8
struct svc { char exec[80]; int pid; int respawn; };
static struct svc svcs[MAXSVC];
static int nsvc;

static void svc_add(const char *exec, int respawn) {
    if (nsvc >= MAXSVC) return;
    int i = 0;
    for (; exec[i] && i < (int)sizeof(svcs[0].exec) - 1; i++) svcs[nsvc].exec[i] = exec[i];
    svcs[nsvc].exec[i] = '\0';
    svcs[nsvc].respawn = respawn;
    svcs[nsvc].pid = -1;
    nsvc++;
}

static void svc_spawn(int i) {
    int pid = fork();
    if (pid == 0) {
        char *av[] = { svcs[i].exec, 0 };
        execve(svcs[i].exec, av, 0);
        _exit(127);
    }
    svcs[i].pid = pid;
}

/* Parse one service .conf file's contents for exec= and respawn= lines. */
static void svc_parse(const char *buf) {
    char exec[80]; int el = 0, respawn = 0, have = 0;
    const char *p = buf;
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        if (has_prefix(line, "exec=")) {
            const char *v = line + 5;
            el = 0;
            while (v < p && el < (int)sizeof(exec) - 1) exec[el++] = *v++;
            exec[el] = '\0'; have = 1;
        } else if (has_prefix(line, "respawn=1")) {
            respawn = 1;
        }
        if (*p == '\n') p++;
    }
    if (have) svc_add(exec, respawn);
}

static void load_services(void) {
    mkdir("/etc/services.d", 0755);
    int fd = open("/etc/services.d", O_RDONLY);
    if (fd < 0) return;
    char dbuf[1024];
    long n;
    while ((n = getdents64(fd, dbuf, sizeof(dbuf))) > 0) {
        long off = 0;
        while (off < n) {
            struct dirent64 *d = (struct dirent64 *)(dbuf + off);
            if (d->d_name[0] != '.' && ends_with(d->d_name, ".conf")) {
                char path[128];
                int k = 0;
                const char *pre = "/etc/services.d/";
                for (int i = 0; pre[i]; i++) path[k++] = pre[i];
                for (int i = 0; d->d_name[i] && k < (int)sizeof(path) - 1; i++) path[k++] = d->d_name[i];
                path[k] = '\0';
                int cf = open(path, O_RDONLY);
                if (cf >= 0) {
                    char cb[512];
                    int r = (int)read(cf, cb, sizeof(cb) - 1);
                    if (r > 0) { cb[r] = '\0'; svc_parse(cb); }
                    close(cf);
                }
            }
            off += d->d_reclen;
        }
    }
    close(fd);
}

/* Read /etc/lariat.conf and print the message-of-the-day, if any. */
static void load_config(void) {
    int fd = open("/etc/lariat.conf", O_RDONLY);
    if (fd < 0) return;
    char buf[512];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    const char *p = buf;
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        if (has_prefix(line, "motd=")) {
            puts1("init: ");
            write(STDOUT_FILENO, line + 5, (size_t)(p - (line + 5)));
            puts1("\n");
        }
        if (*p == '\n') p++;
    }
}

/* Run /bin/lpkg with the given argv, returning its exit status (or -1). */
static int run_lpkg(char *const argv[]) {
    int c = fork();
    if (c < 0) return -1;
    if (c == 0) {
        execve("/bin/lpkg", argv, 0);
        _exit(127);
    }
    int st = -1;
    if (waitpid(c, &st, 0) != c) return -1;
    return WEXITSTATUS(st);
}

/* fork/execve `path` with argv, wait, return its exit status (-1 on error). */
static int run_prog(const char *path, char *const argv[]) {
    int c = fork();
    if (c < 0) return -1;
    if (c == 0) {
        execve(path, argv, 0);
        _exit(127);
    }
    int st = -1;
    if (waitpid(c, &st, 0) != c) return -1;
    return WEXITSTATUS(st);
}

/* Read the whole of `path` into buf (NUL-terminated); returns length or -1. */
static int slurp_file(const char *path, char *buf, int bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int off = 0;
    for (;;) {
        if (off + 1 >= bufsz) break;
        long n = read(fd, buf + off, bufsz - 1 - off);
        if (n <= 0) break;
        off += (int)n;
    }
    close(fd);
    buf[off] = '\0';
    return off;
}

/* True if `hay` contains the substring `needle`. */
static int contains(const char *hay, const char *needle) {
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*n && *h == *n) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

static int str_eq2(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
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

    /* 5. Phase M IPC ports: a named port round-trips a datagram across fork. */
    int port_ok = 1;
    int port = (int)syscall1(SYS_LARIAT_PORT_CREATE, (long)"test.echo");
    if (port < 0) {
        port_ok = 0;
    } else {
        int c = fork();
        if (c == 0) {
            int cp = (int)syscall1(SYS_LARIAT_PORT_OPEN, (long)"test.echo");
            if (cp >= 0) syscall3(SYS_LARIAT_PORT_SEND, cp, (long)"ping", 4);
            _exit(0);
        }
        char buf[16];
        long n = syscall4(SYS_LARIAT_PORT_RECV, port, (long)buf, (long)sizeof(buf), 0);
        if (n != 4 || buf[0] != 'p' || buf[3] != 'g') port_ok = 0;
        int st = -1;
        waitpid(c, &st, 0);
    }
    puts1(port_ok ? "[stress] ipc ports: ok\n" : "[stress] ipc ports: FAILED\n");

    /* 6. Phase 2 package manager: install/list/info/remove a tiny package. */
    int pkg_ok = 1;
    static const char demo_pkg[] =
        "LPKG1\n"
        "name=demo\n"
        "version=1.0.0\n"
        "arch=x86_64\n"
        "deps=\n"
        "desc=lpkg self-test package\n"
        "%FILES\n"
        "0644 3 usr/share/demo/hello.txt\n"
        "%DATA\n"
        "hi\n";
    int wf = open("/demo.lpkg", O_WRONLY | O_CREAT | O_TRUNC);
    if (wf < 0 || write(wf, demo_pkg, sizeof(demo_pkg) - 1) != (ssize_t)(sizeof(demo_pkg) - 1)) {
        pkg_ok = 0;
    }
    if (wf >= 0) close(wf);

    if (pkg_ok) {
        char *a_install[] = { "lpkg", "install", "/demo.lpkg", 0 };
        char *a_list[]    = { "lpkg", "list", 0 };
        char *a_info[]    = { "lpkg", "info", "demo", 0 };
        char *a_remove[]  = { "lpkg", "remove", "demo", 0 };
        if (run_lpkg(a_install) != 0) pkg_ok = 0;
        /* Installed file must exist with the right contents. */
        if (pkg_ok) {
            int rf = open("/usr/share/demo/hello.txt", O_RDONLY);
            char rb[4] = {0};
            if (rf < 0 || read(rf, rb, 3) != 3 || rb[0] != 'h' || rb[1] != 'i') pkg_ok = 0;
            if (rf >= 0) close(rf);
        }
        if (pkg_ok && run_lpkg(a_list) != 0) pkg_ok = 0;
        if (pkg_ok && run_lpkg(a_info) != 0) pkg_ok = 0;
        if (pkg_ok && run_lpkg(a_remove) != 0) pkg_ok = 0;
        /* After removal the file must be gone. */
        if (pkg_ok) {
            int rf = open("/usr/share/demo/hello.txt", O_RDONLY);
            if (rf >= 0) { pkg_ok = 0; close(rf); }
        }
    }
    puts1(pkg_ok ? "[stress] lpkg: ok\n" : "[stress] lpkg: FAILED\n");

    /* 7. Phase 3 window-server protocol: run the exact CONNECT->CONNECTED and
     *    CREATE_WIN->CREATED handshake (wsproto.h over IPC ports) that gui.c
     *    and the ws client library use, with this process acting as server. */
    int ws_ok = 1;
    /* Use a test-only bootstrap name: PID 1 lives forever and a named port has
     * no destructor, so squatting the real WS_BOOTSTRAP ("lariat.wm") here would
     * permanently prevent the actual windowserver from claiming it. */
    const char *WS_TEST_BOOT = "ws.selftest.wm";
    int wsrv = (int)syscall1(SYS_LARIAT_PORT_CREATE, (long)WS_TEST_BOOT);
    if (wsrv < 0) {
        ws_ok = 0;
    } else {
        int wc = fork();
        if (wc == 0) {
            /* Client side. */
            int ev = (int)syscall1(SYS_LARIAT_PORT_CREATE, (long)"ws.ev.test");
            int sv = (int)syscall1(SYS_LARIAT_PORT_OPEN, (long)WS_TEST_BOOT);
            if (ev < 0 || sv < 0) _exit(1);
            ws_msg_t m;
            for (unsigned k = 0; k < sizeof(m); k++) ((char *)&m)[k] = 0;
            m.op = WS_CONNECT;
            const char *en = "ws.ev.test";
            int i = 0;
            for (; en[i]; i++) m.str[i] = en[i];
            m.str[i] = 0;
            syscall3(SYS_LARIAT_PORT_SEND, sv, (long)&m, sizeof(m));
            /* await CONNECTED */
            if (syscall4(SYS_LARIAT_PORT_RECV, ev, (long)&m, sizeof(m), 0) < 0) _exit(2);
            if (m.op != WS_EV_CONNECTED) _exit(3);
            unsigned cid = m.a;
            /* create a window */
            for (unsigned k = 0; k < sizeof(m); k++) ((char *)&m)[k] = 0;
            m.op = WS_CREATE_WIN; m.client_id = cid; m.w = 100; m.h = 80;
            syscall3(SYS_LARIAT_PORT_SEND, sv, (long)&m, sizeof(m));
            if (syscall4(SYS_LARIAT_PORT_RECV, ev, (long)&m, sizeof(m), 0) < 0) _exit(4);
            _exit(m.op == WS_EV_CREATED && m.a != 0 ? 0 : 5);
        }
        /* Server side: handle CONNECT, then CREATE_WIN. */
        ws_msg_t m;
        if (syscall4(SYS_LARIAT_PORT_RECV, wsrv, (long)&m, sizeof(m), 0) < 0 ||
            m.op != WS_CONNECT) {
            ws_ok = 0;
        } else {
            int cev = (int)syscall1(SYS_LARIAT_PORT_OPEN, (long)m.str);
            ws_msg_t r;
            for (unsigned k = 0; k < sizeof(r); k++) ((char *)&r)[k] = 0;
            r.op = WS_EV_CONNECTED; r.a = 7;
            syscall3(SYS_LARIAT_PORT_SEND, cev, (long)&r, sizeof(r));
            if (syscall4(SYS_LARIAT_PORT_RECV, wsrv, (long)&m, sizeof(m), 0) < 0 ||
                m.op != WS_CREATE_WIN) {
                ws_ok = 0;
            } else {
                for (unsigned k = 0; k < sizeof(r); k++) ((char *)&r)[k] = 0;
                r.op = WS_EV_CREATED; r.a = 1; r.win_id = 1;
                syscall3(SYS_LARIAT_PORT_SEND, cev, (long)&r, sizeof(r));
            }
        }
        int st = -1;
        if (waitpid(wc, &st, 0) != wc || WEXITSTATUS(st) != 0) ws_ok = 0;
    }
    puts1(ws_ok ? "[stress] ws protocol: ok\n" : "[stress] ws protocol: FAILED\n");

    /* 8. Phase 4 procfs: read /proc/version, set hostname via /proc and confirm
     *    uname() reflects the change. */
    int proc_ok = 1;
    {
        int vf = open("/proc/version", O_RDONLY);
        char vb[8] = {0};
        if (vf < 0 || read(vf, vb, 6) != 6 ||
            vb[0] != 'L' || vb[1] != 'a' || vb[2] != 'r') proc_ok = 0;
        if (vf >= 0) close(vf);

        int hf = open("/proc/sys/kernel/hostname", O_WRONLY | O_TRUNC);
        const char *hn = "lariatbox";
        if (hf < 0 || write(hf, hn, 9) != 9) proc_ok = 0;
        if (hf >= 0) close(hf);

        struct utsname u;
        if (proc_ok && uname(&u) == 0) {
            const char *e = "lariatbox";
            for (int i = 0; i < 9; i++) if (u.nodename[i] != e[i]) proc_ok = 0;
            if (u.nodename[9] != '\0') proc_ok = 0;
        } else {
            proc_ok = 0;
        }
    }
    puts1(proc_ok ? "[stress] procfs: ok\n" : "[stress] procfs: FAILED\n");

    /* 9. Phase 4 accounts: stronger hash + useradd/userdel lifecycle.
     *    (a) crypt-lite v2 is deterministic, salted ("$L2$..."), verifiable,
     *        and rejects a wrong key; (b) useradd adds a user to the live and
     *        persistent databases, userdel removes it. */
    int acct_ok = 1;
    {
        char h1[96], h2[96], h3[96];
        crypt_lite("s3cret", "ab", h1, sizeof(h1));
        crypt_lite("s3cret", "ab", h2, sizeof(h2));
        crypt_lite("s3cret", h1, h3, sizeof(h3));     /* verify path: re-hash from stored */
        if (!str_eq2(h1, h2)) acct_ok = 0;            /* deterministic */
        if (!str_eq2(h1, h3)) acct_ok = 0;            /* salt recovered from hash */
        if (!(h1[0] == '$' && h1[1] == 'L' && h1[2] == '2' && h1[3] == '$')) acct_ok = 0;
        char hw[96];
        crypt_lite("wrong", h1, hw, sizeof(hw));
        if (str_eq2(h1, hw)) acct_ok = 0;             /* wrong key must differ */
    }
    if (acct_ok) {
        char *a_add[] = { "useradd", "-c", "Test User", "tester", 0 };
        char *a_del[] = { "userdel", "tester", 0 };
        if (run_prog("/bin/useradd", a_add) != 0) acct_ok = 0;

        static char buf[16384];
        if (acct_ok) {
            if (slurp_file("/etc/passwd", buf, sizeof(buf)) < 0 ||
                !contains(buf, "tester:x:")) acct_ok = 0;
        }
        if (acct_ok) {  /* must also have been persisted to /var/etc */
            if (slurp_file("/var/etc/passwd", buf, sizeof(buf)) < 0 ||
                !contains(buf, "tester:x:")) acct_ok = 0;
        }
        if (acct_ok && run_prog("/bin/userdel", a_del) != 0) acct_ok = 0;
        if (acct_ok) {  /* gone from both live and persistent passwd */
            if (slurp_file("/etc/passwd", buf, sizeof(buf)) < 0 ||
                contains(buf, "tester:x:")) acct_ok = 0;
            if (slurp_file("/var/etc/passwd", buf, sizeof(buf)) >= 0 &&
                contains(buf, "tester:x:")) acct_ok = 0;
        }
    }
    puts1(acct_ok ? "[stress] accounts: ok\n" : "[stress] accounts: FAILED\n");

    /* 10. Phase 5 on-device compile: lcc turns a C-subset source into a native
     *     static ELF; we then exec it and check its computed exit code (42) to
     *     prove the compile-and-run-on-device toolchain milestone end to end. */
    int cc_ok = 1;
    {
        static const char prog[] =
            "int main(void) {\n"
            "    int a = 6;\n"
            "    int b = 7;\n"
            "    write(\"[lcc] hello from a compiled program\\n\");\n"
            "    return a * b;   // 42\n"
            "}\n";
        int sf = open("/tmp/t.c", O_WRONLY | O_CREAT | O_TRUNC);
        if (sf < 0 || write(sf, prog, sizeof(prog) - 1) != (ssize_t)(sizeof(prog) - 1)) cc_ok = 0;
        if (sf >= 0) close(sf);

        if (cc_ok) {
            char *a_cc[] = { "lcc", "/tmp/t.c", "/tmp/t.out", 0 };
            if (run_prog("/bin/lcc", a_cc) != 0) cc_ok = 0;
        }
        if (cc_ok) {
            char *a_run[] = { "/tmp/t.out", 0 };
            int rc = run_prog("/tmp/t.out", a_run);   /* should compute 6*7 = 42 */
            if (rc != 42) cc_ok = 0;
        }
    }
    puts1(cc_ok ? "[stress] lcc compile+run: ok\n" : "[stress] lcc compile+run: FAILED\n");

    /* ext4 read-write: create a file on the /var data volume, write/read it
     * back, then make a subdirectory, create a file inside it, and clean up.
     * Proves the ext4 write path (block/inode bitmap allocation, extent append,
     * dir entry insert/remove, inode writeback). Skipped if /var is absent. */
    {
        int e_ok = 1, have_var = 0;
        { int pf = open("/var/dyn.pkg", O_RDONLY);
          if (pf >= 0) { have_var = 1; close(pf); } }
        if (have_var) {
            const char *msg = "ext4-write-roundtrip-ok";
            size_t mlen = strlen(msg);
            int fd = open("/var/wtest.txt", O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0 || write(fd, msg, mlen) != (ssize_t)mlen) e_ok = 0;
            if (fd >= 0) close(fd);

            char rb[64];
            int rf = open("/var/wtest.txt", O_RDONLY);
            if (rf < 0) e_ok = 0;
            else {
                int n = (int)read(rf, rb, sizeof(rb) - 1);
                close(rf);
                if (n != (int)mlen) e_ok = 0;
                else { for (size_t i = 0; i < mlen; i++)
                           if (rb[i] != msg[i]) { e_ok = 0; break; } }
            }

            if (e_ok && mkdir("/var/wdir", 0755) == 0) {
                int df = open("/var/wdir/inside.txt", O_WRONLY | O_CREAT | O_TRUNC);
                if (df < 0 || write(df, "hi", 2) != 2) e_ok = 0;
                if (df >= 0) close(df);
                int cf = open("/var/wdir/inside.txt", O_RDONLY);
                if (cf < 0) e_ok = 0;
                else { char cb[4]; int n = (int)read(cf, cb, 3); close(cf);
                       if (n != 2 || cb[0] != 'h' || cb[1] != 'i') e_ok = 0; }
                unlink("/var/wdir/inside.txt");
                rmdir("/var/wdir");
            } else if (e_ok) {
                e_ok = 0;   /* mkdir failed */
            }

            puts1(e_ok ? "[stress] ext4 write: ok\n"
                       : "[stress] ext4 write: FAILED\n");
        } else {
            puts1("[stress] ext4 write: skipped (no /var)\n");
        }
    }

    /* Dynamic musl toolchain: install a package built by the x86_64-linux-musl
     * cross gcc (the musl loader /lib/ld-musl-x86_64.so.1 + a PIE C binary at
     * /bin/dyntest), then exec the binary. The package is delivered on the /var
     * data volume as /var/dyn.pkg, so this also exercises reading a large file
     * off the ext4 filesystem. Success (exit 42 + its banner) proves the kernel
     * ELF loader loads PT_INTERP/ld.so and runs a real cross-compiled dynamic
     * executable - the end-to-end gcc/g++-compatibility milestone. Falls back to
     * the legacy FAT32 copy, and is skipped if neither package is present. */
    {
        int dyn_ok = 1;
        const char *pkg = 0;
        { int pf = open("/var/dyn.pkg", O_RDONLY);
          if (pf >= 0) { pkg = "/var/dyn.pkg"; close(pf); } }
        if (!pkg) { int pf = open("/mnt/legacy/dyn.pkg", O_RDONLY);
          if (pf >= 0) { pkg = "/mnt/legacy/dyn.pkg"; close(pf); } }
        if (pkg) {
            puts1("[dyn] installing "); puts1(pkg); puts1(" ...\n");
            char *a_inst[] = { "lpkg", "install", (char *)pkg, "--force", 0 };
            if (run_prog("/bin/lpkg", a_inst) != 0) dyn_ok = 0;
            puts1(dyn_ok ? "[dyn] installed; exec /bin/dyntest ...\n"
                         : "[dyn] install FAILED\n");
            if (dyn_ok) {
                char *a_run[] = { "/bin/dyntest", 0 };
                int rc = run_prog("/bin/dyntest", a_run);
                puts1("[dyn] dyntest rc="); put_int(rc); puts1("\n");
                if (rc != 42) dyn_ok = 0;
            }
            puts1(dyn_ok ? "[stress] dyn musl exec (from ext4): ok\n"
                         : "[stress] dyn musl exec: FAILED\n");
        } else {
            puts1("[stress] dyn musl exec: skipped (no dyn.pkg)\n");
        }
    }

    /* Existing-application compatibility (roadmap N7): install MicroPython
     * (cross-compiled against musl, minimal variant) from /var/mpy.pkg and run
     * a real Python snippet through it.  This proves Lariat can host a genuine
     * third-party language runtime - a dynamic musl PIE that exercises the
     * loader, the libc, and a much wider syscall surface than dyntest. */
    {
        int mpy_ok = 0;
        int pf = open("/var/mpy.pkg", O_RDONLY);
        if (pf >= 0) {
            close(pf);
            puts1("[mpy] installing /var/mpy.pkg ...\n");
            char *a_inst[] = { "lpkg", "install", "/var/mpy.pkg", "--force", 0 };
            if (run_prog("/bin/lpkg", a_inst) == 0) {
                puts1("[mpy] running: python -c \"print(...)\" ...\n");
                char *a_run[] = { "/bin/micropython", "-c",
                    "print('[mpy] hello from micropython on lariat');"
                    "print('[mpy] 6*7 =', 6*7);"
                    "print('[mpy] sum0..99 =', sum(range(100)))", 0 };
                int rc = run_prog("/bin/micropython", a_run);
                puts1("[mpy] micropython rc="); put_int(rc); puts1("\n");
                mpy_ok = (rc == 0);
            } else {
                puts1("[mpy] install FAILED\n");
            }
            puts1(mpy_ok ? "[stress] python (micropython): ok\n"
                         : "[stress] python (micropython): FAILED\n");
        } else {
            puts1("[stress] python (micropython): skipped (no mpy.pkg)\n");
        }
    }

    /* N6: show the kernel mount table (proves /proc/mounts + fstab parsing). */
    {
        char mb[512];
        int n = slurp_file("/proc/mounts", mb, sizeof(mb) - 1);
        if (n > 0) {
            mb[n] = '\0';
            puts1("[mounts] /proc/mounts:\n");
            puts1(mb);
            puts1("[stress] proc mounts: ok\n");
        } else {
            puts1("[stress] proc mounts: FAILED\n");
        }
    }

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

    /* Phase 4: apply configuration and start managed services.  The console
     * login session is the first (respawn) service; service .conf files may
     * add more.  PID 1 then supervises: reap children and respawn services. */
    load_config();
    svc_add("/bin/login", 1);     /* console login: always respawn */
    load_services();

    for (int i = 0; i < nsvc; i++) svc_spawn(i);

    for (;;) {
        int st;
        int pid = waitpid(-1, &st, 0);
        if (pid < 0) continue;
        for (int i = 0; i < nsvc; i++) {
            if (svcs[i].pid == pid) {
                if (svcs[i].respawn) svc_spawn(i);
                else svcs[i].pid = -1;
                break;
            }
        }
    }
    return 0;
}
