/* su - switch user.  Installed setuid-root so an unprivileged caller can
 * (after authenticating) become another user.  When the real uid is already
 * root no password is required. */

#include "libc/unistd.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/pwd.h"
#include "libc/termios.h"

static int read_pass(const char *prompt, char *buf, int sz) {
    fputs(prompt, STDOUT_FILENO);
    struct termios t, saved;
    int changed = 0;
    if (tcgetattr(STDIN_FILENO, &t) == 0) {
        saved = t;
        t.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        changed = 1;
    }
    int n = (int)read(STDIN_FILENO, buf, sz - 1);
    if (changed) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        write(STDOUT_FILENO, "\n", 1);
    }
    if (n <= 0) return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n] = '\0';
    return n;
}

int main(int argc, char **argv) {
    const char *target = (argc > 1) ? argv[1] : "root";

    struct passwd *pw = getpwnam(target);
    if (!pw) {
        fprintf(STDERR_FILENO, "su: unknown user %s\n", target);
        return 1;
    }

    /* A non-root caller must prove they know the target's password. */
    if (getuid() != 0) {
        char pass[80], stored[96];
        if (read_pass("Password: ", pass, sizeof(pass)) < 0) return 1;
        if (shadow_get(target, stored, sizeof(stored)) != 0 ||
            (stored[0] != '\0' && strcmp(crypt(pass, stored), stored) != 0)) {
            fputs("su: Authentication failure\n", STDOUT_FILENO);
            return 1;
        }
    }

    gid_t g = pw->pw_gid;
    setgid(pw->pw_gid);
    setgroups(1, &g);
    if (setuid(pw->pw_uid) != 0) {
        fputs("su: cannot set uid\n", STDOUT_FILENO);
        return 1;
    }

    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    chdir(pw->pw_dir);

    const char *sh = (pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell
                                                       : "/bin/sh";
    char *av[] = { (char *)sh, 0 };
    execve(sh, av, environ);
    fputs("su: failed to exec shell\n", STDOUT_FILENO);
    return 1;
}
