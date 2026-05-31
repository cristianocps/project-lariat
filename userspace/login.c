/* login - authenticate a user against /etc/passwd + /etc/shadow, drop
 * privileges to that user, and exec their login shell.
 *
 * init(8) spawns this as root on the console and respawns it when the session
 * ends, so the loop here only needs to retry on bad credentials. */

#include "libc/unistd.h"
#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/pwd.h"
#include "libc/termios.h"

/* Prompt and read one line from stdin.  When echo==0 the terminal's ECHO flag
 * is cleared for the duration (password entry).  Returns length or -1. */
static int read_field(const char *prompt, char *buf, int sz, int echo) {
    fputs(prompt, STDOUT_FILENO);

    struct termios t, saved;
    int changed = 0;
    if (!echo && tcgetattr(STDIN_FILENO, &t) == 0) {
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

static int verify(const char *user, const char *pass, struct passwd **out) {
    struct passwd *pw = getpwnam(user);
    if (!pw) return 0;
    *out = pw;

    char stored[96];
    if (shadow_get(user, stored, sizeof(stored)) != 0) return 0;
    if (stored[0] == '\0') return pass[0] == '\0';   /* passwordless account */

    char *h = crypt(pass, stored);
    return strcmp(h, stored) == 0;
}

int main(void) {
    char user[64], pass[80];

    for (;;) {
        if (read_field("\nlariat login: ", user, sizeof(user), 1) <= 0)
            continue;
        if (read_field("Password: ", pass, sizeof(pass), 0) < 0)
            continue;

        struct passwd *pw = 0;
        if (!verify(user, pass, &pw)) {
            fputs("Login incorrect\n", STDOUT_FILENO);
            continue;
        }

        /* Drop privileges: groups + gid first (still root), then uid. */
        gid_t g = pw->pw_gid;
        setgid(pw->pw_gid);
        setgroups(1, &g);
        if (setuid(pw->pw_uid) != 0) {
            fputs("login: cannot set uid\n", STDOUT_FILENO);
            continue;
        }

        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1);
        setenv("LOGNAME", pw->pw_name, 1);

        /* Build a Unix-style search PATH for the session (cf. Debian
         * /etc/profile, macOS path_helper).  System-wide executables come from
         * the *bin trees; root additionally gets the *sbin trees; an ordinary
         * user gets their per-user ~/.local/bin and ~/bin prepended so they
         * can install commands without touching the system image. */
        char pathbuf[256];
        int n = 0;
        if (pw->pw_uid != 0) {
            /* "<home>/.local/bin:<home>/bin:" */
            const char *suffixes[] = { "/.local/bin:", "/bin:" };
            for (unsigned s = 0; s < 2; s++) {
                for (const char *h = pw->pw_dir; *h && n < (int)sizeof(pathbuf) - 1; h++)
                    pathbuf[n++] = *h;
                for (const char *sf = suffixes[s]; *sf && n < (int)sizeof(pathbuf) - 1; sf++)
                    pathbuf[n++] = *sf;
            }
        }
        const char *sysdirs = (pw->pw_uid == 0)
            ? "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
            : "/usr/local/bin:/usr/bin:/bin";
        for (const char *d = sysdirs; *d && n < (int)sizeof(pathbuf) - 1; d++)
            pathbuf[n++] = *d;
        pathbuf[n] = '\0';
        setenv("PATH", pathbuf, 1);
        chdir(pw->pw_dir);

        printf("\nWelcome to Lariat, %s.\n", pw->pw_name);

        const char *sh = (pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell
                                                           : "/bin/sh";
        char *av[] = { (char *)sh, 0 };
        execve(sh, av, environ);
        fputs("login: failed to exec shell\n", STDOUT_FILENO);
        _exit(1);
    }
    return 0;
}
