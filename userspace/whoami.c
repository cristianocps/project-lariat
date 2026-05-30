/* whoami - print the effective user name. */

#include "libc/unistd.h"
#include "libc/stdio.h"
#include "libc/pwd.h"

int main(void) {
    struct passwd *pw = getpwuid(geteuid());
    if (pw) {
        printf("%s\n", pw->pw_name);
    } else {
        printf("%d\n", (int)geteuid());
    }
    return 0;
}
