/* id - print real/effective user and group identity. */

#include "libc/unistd.h"
#include "libc/stdio.h"
#include "libc/pwd.h"

static const char *uname_of(uid_t u) {
    struct passwd *pw = getpwuid(u);
    return pw ? pw->pw_name : 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uid_t ruid = getuid(), euid = geteuid();
    gid_t rgid = getgid(), egid = getegid();

    const char *un = uname_of(ruid);
    if (un) printf("uid=%d(%s)", (int)ruid, un);
    else    printf("uid=%d", (int)ruid);

    printf(" gid=%d", (int)rgid);

    if (euid != ruid) {
        const char *en = uname_of(euid);
        if (en) printf(" euid=%d(%s)", (int)euid, en);
        else    printf(" euid=%d", (int)euid);
    }
    if (egid != rgid) printf(" egid=%d", (int)egid);

    gid_t grps[32];
    int ng = getgroups(32, grps);
    if (ng > 0) {
        printf(" groups=");
        for (int i = 0; i < ng; i++) printf("%s%d", i ? "," : "", (int)grps[i]);
    }
    printf("\n");
    return 0;
}
