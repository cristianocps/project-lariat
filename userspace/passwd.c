/* passwd - change a user's password in /etc/shadow.  Installed setuid-root so
 * an ordinary user can rewrite the (root-owned, 0600) shadow file after proving
 * they know their current password.  Root may change any account. */

#include "libc/unistd.h"
#include "libc/stdio.h"
#include "libc/string.h"
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

static void mksalt(char *s) {
    static const char tab[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned seed = (unsigned)getpid() * 2654435761u + (unsigned)gettid();
    for (int i = 0; i < 4; i++) {
        seed = seed * 1103515245u + 12345u;
        s[i] = tab[(seed >> 16) % 36];
    }
    s[4] = '\0';
}

int main(int argc, char **argv) {
    uid_t ruid = getuid();

    /* getpwuid()/getpwnam() share a static return buffer, so capture the names
     * we need into local storage before issuing another lookup. */
    char self_name[64] = {0};
    struct passwd *self = getpwuid(ruid);
    if (self) strncpy(self_name, self->pw_name, sizeof(self_name) - 1);

    char target[64] = {0};
    if (argc > 1) strncpy(target, argv[1], sizeof(target) - 1);
    else          strncpy(target, self_name, sizeof(target) - 1);

    if (target[0] == '\0') {
        fputs("passwd: cannot determine user\n", STDOUT_FILENO);
        return 1;
    }
    if (!getpwnam(target)) {
        fprintf(STDERR_FILENO, "passwd: unknown user %s\n", target);
        return 1;
    }

    if (ruid != 0) {
        if (self_name[0] == '\0' || strcmp(target, self_name) != 0) {
            fputs("passwd: you may only change your own password\n",
                  STDOUT_FILENO);
            return 1;
        }
        char old[80], stored[96];
        if (read_pass("Current password: ", old, sizeof(old)) < 0) return 1;
        if (shadow_get(target, stored, sizeof(stored)) == 0 &&
            stored[0] != '\0' &&
            strcmp(crypt(old, stored), stored) != 0) {
            fputs("passwd: Authentication failure\n", STDOUT_FILENO);
            return 1;
        }
    }

    char n1[80], n2[80];
    if (read_pass("New password: ", n1, sizeof(n1)) < 0) return 1;
    if (read_pass("Retype new password: ", n2, sizeof(n2)) < 0) return 1;
    if (strcmp(n1, n2) != 0) {
        fputs("passwd: passwords do not match\n", STDOUT_FILENO);
        return 1;
    }

    char salt[8];
    mksalt(salt);
    char *h = crypt(n1, salt);
    if (shadow_set(target, h) != 0) {
        fputs("passwd: failed to update /etc/shadow\n", STDOUT_FILENO);
        return 1;
    }
    etc_sync("shadow");   /* make the change survive a reboot */
    printf("passwd: password updated successfully\n");
    return 0;
}
