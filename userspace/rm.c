/* rm - remove files (and directories with -r). */

#include "libc/unistd.h"
#include "libc/errno.h"
#include "libc/string.h"
#include "libc/stdio.h"
#include "libc/dirent.h"
#include "libc/sys/stat.h"

static int recursive = 0;

static int rm_path(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(STDERR_FILENO, "rm: %s: error %d\n", path, errno);
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            fprintf(STDERR_FILENO, "rm: %s: is a directory\n", path);
            return 1;
        }
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                char child[256];
                snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                rm_path(child);
            }
            closedir(d);
        }
        if (rmdir(path) < 0) {
            fprintf(STDERR_FILENO, "rm: %s: error %d\n", path, errno);
            return 1;
        }
        return 0;
    }
    if (unlink(path) < 0) {
        fprintf(STDERR_FILENO, "rm: %s: error %d\n", path, errno);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int rc = 0, files = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-rf") == 0 ||
            strcmp(argv[i], "-R") == 0) {
            recursive = 1;
            continue;
        }
        files++;
        rc |= rm_path(argv[i]);
    }
    if (!files) {
        fputs("usage: rm [-r] FILE...\n", STDERR_FILENO);
        return 1;
    }
    return rc;
}
