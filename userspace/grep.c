/* grep - print lines matching a substring pattern (optionally with -n). */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"
#include "libc/string.h"
#include "libc/stdio.h"

static int show_num = 0;

static int contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (const char *h = hay; *h; h++) {
        if (strncmp(h, needle, nl) == 0) return 1;
    }
    return 0;
}

static int grep_fd(int fd, const char *pat, const char *label) {
    freader fr;
    freader_init(&fr, fd);
    char line[2048];
    int lineno = 0, matched = 0;
    while (freader_getline(&fr, line, sizeof(line)) >= 0) {
        lineno++;
        if (contains(line, pat)) {
            matched = 1;
            if (label) fprintf(STDOUT_FILENO, "%s:", label);
            if (show_num) fprintf(STDOUT_FILENO, "%d:", lineno);
            puts(line);
        }
    }
    return matched;
}

int main(int argc, char **argv) {
    const char *pat = 0;
    int first_file = 0;
    for (int i = 1; i < argc; i++) {
        if (!pat && strcmp(argv[i], "-n") == 0) { show_num = 1; continue; }
        if (!pat) { pat = argv[i]; continue; }
        first_file = i;
        break;
    }
    if (!pat) {
        fputs("usage: grep [-n] PATTERN [FILE...]\n", STDERR_FILENO);
        return 2;
    }

    int any = 0;
    if (first_file == 0) {
        any = grep_fd(STDIN_FILENO, pat, 0);
    } else {
        int multi = (argc - first_file) > 1;
        for (int i = first_file; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                fprintf(STDERR_FILENO, "grep: %s: error %d\n", argv[i], errno);
                continue;
            }
            any |= grep_fd(fd, pat, multi ? argv[i] : 0);
            close(fd);
        }
    }
    return any ? 0 : 1;
}
