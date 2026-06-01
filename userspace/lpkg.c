/* lpkg - the Lariat package manager.
 *
 * Implements local install/remove/list/info over a simple, host-creatable
 * package format ("LPKG1") and an on-disk database on the persistent ext4
 * /var data volume.  See docs/adr/0003-package-format-and-lpkg.md and scripts/mklpkg.sh.
 *
 * Package format (text header + raw payload):
 *
 *     LPKG1\n
 *     name=<name>\n
 *     version=<ver>\n
 *     arch=<arch>\n
 *     deps=<comma-separated, may be empty>\n
 *     desc=<one line>\n
 *     %FILES\n
 *     <octal-mode> <decimal-size> <relative/dest/path>\n   (repeated)
 *     %DATA\n
 *     <raw bytes: each file's contents concatenated in %FILES order>
 *
 * On-disk DB (persistent, survives reboot on the /var data volume):
 *
 *     /var/lib/lpkg/db/<name>/meta     - the package metadata
 *     /var/lib/lpkg/db/<name>/files    - installed absolute paths, one/line
 *     /var/lib/lpkg/db/<name>/archive  - a copy of the .lpkg for `sync`
 *
 * Installed files land in the live (ramfs) tree at their absolute path (e.g.
 * /usr/bin/foo) with the recorded mode so they are executable; `lpkg sync`
 * re-extracts every recorded package after a reboot (the ramfs tree is
 * volatile while the DB on /var is not).
 */

#include "libc/unistd.h"
#include "libc/fcntl.h"
#include "libc/errno.h"
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/string.h"
#include "libc/sys/stat.h"
#include "libc/dirent.h"
#include "libc/sys/socket.h"
#include "libc/arpa/inet.h"

#define DB_ROOT   "/var/lib/lpkg"
#define DB_DIR    "/var/lib/lpkg/db"
#define LPKG_ARCH "x86_64"
/* Package payload + metadata cap.  Large enough for the native toolchain
 * (gcc's cc1 alone is tens of MiB); the archive is read whole into memory on
 * install, so this also bounds that allocation. */
#define MAX_PKG_SIZE (256u << 20)
/* Max files per package.  The C/C++ headers (musl + gcc intrinsics) and the
 * toolchain push well past a hundred files; 512 keeps headroom while bounding
 * the (stack-resident) struct pkg to ~140 KiB. */
#define MAX_FILES 512

struct pkg_file {
    unsigned int mode;
    unsigned long size;
    char         path[256];   /* absolute, with leading '/' */
    unsigned long offset;     /* into the payload region */
};

struct pkg {
    char name[64];
    char version[32];
    char arch[16];
    char deps[256];
    char desc[256];
    struct pkg_file files[MAX_FILES];
    int  nfiles;
    char *data;          /* whole archive buffer */
    unsigned long len;
    unsigned long payload;  /* byte offset of the %DATA payload */
};

/* ---- small filesystem helpers ------------------------------------------- */

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* mkdir -p for the directory part of an absolute path (or a directory). */
static int mkdir_p(const char *path) {
    char tmp[256];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (!exists(tmp)) mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    if (!exists(tmp)) mkdir(tmp, 0755);
    return 0;
}

/* mkdir -p of the parent directory of a file path. */
static void mkdir_parent(const char *file) {
    char tmp[256];
    strncpy(tmp, file, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return;
    *slash = '\0';
    mkdir_p(tmp);
}

static int write_all(int fd, const void *buf, unsigned long n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return -1;
        p += w; n -= (unsigned long)w;
    }
    return 0;
}

/* Write a buffer to a file, creating parent dirs and setting mode. */
static int install_file(const char *path, const void *buf, unsigned long n,
                        unsigned int mode) {
    mkdir_parent(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    int rc = write_all(fd, buf, n);
    close(fd);
    if (rc == 0) chmod(path, mode);
    return rc;
}

/* Read an entire file into a freshly malloc'd buffer (NUL-terminated). */
static char *read_whole(const char *path, unsigned long *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0 || st.st_size > MAX_PKG_SIZE) {
        close(fd);
        return NULL;
    }
    unsigned long len = (unsigned long)st.st_size;
    char *buf = malloc(len + 1);
    if (!buf) { close(fd); return NULL; }
    unsigned long got = 0;
    while (got < len) {
        ssize_t r = read(fd, buf + got, len - got);
        if (r <= 0) break;
        got += (unsigned long)r;
    }
    close(fd);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

/* ---- archive parsing ----------------------------------------------------- */

static unsigned int parse_octal(const char *s) {
    unsigned int v = 0;
    while (*s >= '0' && *s <= '7') { v = (v << 3) + (unsigned int)(*s - '0'); s++; }
    return v;
}

/* Copy a header value (text up to newline) into dst. */
static void copy_val(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    while (src[i] && src[i] != '\n' && i < dstsz - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Parse the in-memory archive into *p (data pointer is borrowed, not copied). */
static int pkg_parse(struct pkg *p, char *data, unsigned long len) {
    memset(p, 0, sizeof(*p));
    p->data = data;
    p->len = len;
    strcpy(p->arch, LPKG_ARCH);

    if (len < 6 || strncmp(data, "LPKG1\n", 6) != 0) {
        fputs("lpkg: not an LPKG1 archive\n", STDERR_FILENO);
        return -1;
    }

    char *line = data + 6;
    /* metadata lines until %FILES */
    while (line < data + len && !starts_with(line, "%FILES")) {
        if (starts_with(line, "name="))         copy_val(p->name, sizeof(p->name), line + 5);
        else if (starts_with(line, "version=")) copy_val(p->version, sizeof(p->version), line + 8);
        else if (starts_with(line, "arch="))    copy_val(p->arch, sizeof(p->arch), line + 5);
        else if (starts_with(line, "deps="))    copy_val(p->deps, sizeof(p->deps), line + 5);
        else if (starts_with(line, "desc="))    copy_val(p->desc, sizeof(p->desc), line + 5);
        char *nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }
    if (!starts_with(line, "%FILES")) { fputs("lpkg: missing %FILES\n", STDERR_FILENO); return -1; }
    char *nl = strchr(line, '\n');
    if (!nl) return -1;
    line = nl + 1;

    /* file entries until %DATA */
    while (line < data + len && !starts_with(line, "%DATA")) {
        if (p->nfiles >= MAX_FILES) { fputs("lpkg: too many files\n", STDERR_FILENO); return -1; }
        struct pkg_file *f = &p->files[p->nfiles];
        /* "<octal-mode> <decimal-size> <path>" */
        char *q = line;
        f->mode = parse_octal(q);
        while (*q && *q != ' ') q++;
        while (*q == ' ') q++;
        f->size = (unsigned long)atol(q);
        while (*q && *q != ' ') q++;
        while (*q == ' ') q++;
        /* path: prepend '/' so dests are absolute */
        f->path[0] = '/';
        copy_val(f->path + 1, sizeof(f->path) - 1, q);
        p->nfiles++;
        nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }
    if (!starts_with(line, "%DATA")) { fputs("lpkg: missing %DATA\n", STDERR_FILENO); return -1; }
    nl = strchr(line, '\n');
    if (!nl) return -1;
    p->payload = (unsigned long)((nl + 1) - data);

    /* assign payload offsets in order */
    unsigned long off = p->payload;
    for (int i = 0; i < p->nfiles; i++) {
        p->files[i].offset = off;
        off += p->files[i].size;
    }
    if (off > len) { fputs("lpkg: truncated payload\n", STDERR_FILENO); return -1; }
    if (!p->name[0]) { fputs("lpkg: archive has no name\n", STDERR_FILENO); return -1; }
    return 0;
}

/* ---- database ------------------------------------------------------------ */

static void db_pkg_dir(char *out, size_t sz, const char *name) {
    snprintf(out, sz, "%s/%s", DB_DIR, name);
}

static int db_pkg_installed(const char *name) {
    char meta[256];
    snprintf(meta, sizeof(meta), "%s/%s/meta", DB_DIR, name);
    return exists(meta);
}

static int db_ensure(void) {
    mkdir_p(DB_DIR);
    return exists(DB_DIR) ? 0 : -1;
}

/* ---- commands ------------------------------------------------------------ */

/* Extract a parsed package's files into the live tree. */
static int extract_files(struct pkg *p) {
    for (int i = 0; i < p->nfiles; i++) {
        struct pkg_file *f = &p->files[i];
        if (install_file(f->path, p->data + f->offset, f->size, f->mode) != 0) {
            fprintf(STDERR_FILENO, "lpkg: failed to write %s (errno %d)\n", f->path, errno);
            return -1;
        }
    }
    return 0;
}

static int check_deps(struct pkg *p) {
    if (!p->deps[0]) return 0;
    char tmp[256];
    strncpy(tmp, p->deps, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    int missing = 0;
    char *save = NULL;
    for (char *d = strtok_r(tmp, ",", &save); d; d = strtok_r(NULL, ",", &save)) {
        while (*d == ' ') d++;
        if (!*d) continue;
        if (!db_pkg_installed(d)) {
            fprintf(STDERR_FILENO, "lpkg: missing dependency: %s\n", d);
            missing++;
        }
    }
    return missing ? -1 : 0;
}

static int cmd_install(const char *archive, int force) {
    if (db_ensure() != 0) {
        fputs("lpkg: cannot create database (is /var mounted?)\n", STDERR_FILENO);
        return 1;
    }
    unsigned long len = 0;
    char *data = read_whole(archive, &len);
    if (!data) { fprintf(STDERR_FILENO, "lpkg: cannot read %s\n", archive); return 1; }

    struct pkg p;
    if (pkg_parse(&p, data, len) != 0) { free(data); return 1; }

    if (strcmp(p.arch, LPKG_ARCH) != 0 && !force) {
        fprintf(STDERR_FILENO, "lpkg: arch mismatch (%s != %s); use --force\n", p.arch, LPKG_ARCH);
        free(data);
        return 1;
    }
    if (check_deps(&p) != 0 && !force) { free(data); return 1; }

    if (extract_files(&p) != 0) { free(data); return 1; }

    /* Record into the DB. */
    char dir[256], path[256];
    db_pkg_dir(dir, sizeof(dir), p.name);
    mkdir_p(dir);

    snprintf(path, sizeof(path), "%s/meta", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        char meta[640];
        int m = snprintf(meta, sizeof(meta),
                         "name=%s\nversion=%s\narch=%s\ndeps=%s\ndesc=%s\n",
                         p.name, p.version, p.arch, p.deps, p.desc);
        write_all(fd, meta, (unsigned long)m);
        close(fd);
    }

    snprintf(path, sizeof(path), "%s/files", dir);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        for (int i = 0; i < p.nfiles; i++) {
            write_all(fd, p.files[i].path, strlen(p.files[i].path));
            write_all(fd, "\n", 1);
        }
        close(fd);
    }

    /* Installed files land under /usr, which is firmlinked onto the persistent
     * /var data volume, so they survive reboot in place - there is no need to
     * cache the archive in the DB for a boot-time re-extract anymore. */

    printf("installed %s %s (%d file%s)\n", p.name, p.version, p.nfiles,
           p.nfiles == 1 ? "" : "s");
    free(data);
    return 0;
}

static int read_files_list(const char *name, void (*cb)(const char *path)) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/files", DB_DIR, name);
    unsigned long len = 0;
    char *buf = read_whole(path, &len);
    if (!buf) return -1;
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (*line) cb(line);
        if (!nl) break;
        line = nl + 1;
    }
    free(buf);
    return 0;
}

static void unlink_cb(const char *path) { unlink(path); }

static int cmd_remove(const char *name) {
    if (!db_pkg_installed(name)) {
        fprintf(STDERR_FILENO, "lpkg: %s is not installed\n", name);
        return 1;
    }
    read_files_list(name, unlink_cb);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s/meta", DB_DIR, name);    unlink(path);
    snprintf(path, sizeof(path), "%s/%s/files", DB_DIR, name);   unlink(path);
    snprintf(path, sizeof(path), "%s/%s/archive", DB_DIR, name); unlink(path);
    snprintf(path, sizeof(path), "%s/%s", DB_DIR, name);         rmdir(path);
    printf("removed %s\n", name);
    return 0;
}

/* Read a single key from a package's meta file. */
static int meta_get(const char *name, const char *key, char *out, size_t sz) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/meta", DB_DIR, name);
    unsigned long len = 0;
    char *buf = read_whole(path, &len);
    if (!buf) return -1;
    int found = -1;
    char pfx[64];
    snprintf(pfx, sizeof(pfx), "%s=", key);
    char *line = buf;
    while (*line) {
        if (starts_with(line, pfx)) {
            copy_val(out, sz, line + strlen(pfx));
            found = 0;
            break;
        }
        char *nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }
    free(buf);
    return found;
}

static int cmd_list(void) {
    int fd = open(DB_DIR, O_RDONLY);
    if (fd < 0) { puts("no packages installed"); return 0; }
    char buf[2048];
    long n;
    int count = 0;
    while ((n = getdents64(fd, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            struct dirent64 *d = (struct dirent64 *)(buf + off);
            if (d->d_name[0] != '.' && db_pkg_installed(d->d_name)) {
                char ver[32] = "?";
                meta_get(d->d_name, "version", ver, sizeof(ver));
                printf("%s %s\n", d->d_name, ver);
                count++;
            }
            off += d->d_reclen;
        }
    }
    close(fd);
    if (count == 0) puts("no packages installed");
    return 0;
}

static void print_file_cb(const char *path) { printf("  %s\n", path); }

static int cmd_info(const char *name) {
    if (!db_pkg_installed(name)) {
        fprintf(STDERR_FILENO, "lpkg: %s is not installed\n", name);
        return 1;
    }
    char val[256];
    printf("Name:    %s\n", name);
    if (meta_get(name, "version", val, sizeof(val)) == 0) printf("Version: %s\n", val);
    if (meta_get(name, "arch", val, sizeof(val)) == 0)    printf("Arch:    %s\n", val);
    if (meta_get(name, "deps", val, sizeof(val)) == 0 && val[0]) printf("Depends: %s\n", val);
    if (meta_get(name, "desc", val, sizeof(val)) == 0 && val[0]) printf("Desc:    %s\n", val);
    puts("Files:");
    read_files_list(name, print_file_cb);
    return 0;
}

/* Historically `lpkg sync` re-extracted every recorded package into the
 * volatile ramfs tree after a reboot.  Now that /usr is firmlinked onto the
 * persistent /var volume, installed files survive reboot in place and no sync
 * is needed.  The command is kept as a no-op so existing scripts/habits do not
 * break, and reports the package count for reassurance. */
static int cmd_sync(void) {
    int fd = open(DB_DIR, O_RDONLY);
    if (fd < 0) { printf("lpkg: nothing to sync (no package database)\n"); return 0; }
    char buf[2048];
    long n;
    int count = 0;
    while ((n = getdents64(fd, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            struct dirent64 *d = (struct dirent64 *)(buf + off);
            if (d->d_name[0] != '.' && db_pkg_installed(d->d_name)) count++;
            off += d->d_reclen;
        }
    }
    close(fd);
    printf("lpkg: %d package%s installed on persistent /usr (no sync needed)\n",
           count, count == 1 ? "" : "s");
    return 0;
}

/* ---- network repository (HTTP) ------------------------------------------ */

/* Download http://<ip>:<port><path> into outfile, stripping HTTP headers.
 * Returns 0 on a 200 response with a saved body, -1 otherwise. */
static int http_download(const char *ip, int port, const char *path,
                         const char *outfile) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fputs("lpkg: socket failed\n", STDERR_FILENO); return -1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = inet_addr(ip);
    if (sa.sin_addr.s_addr == INADDR_NONE) {
        fputs("lpkg: bad repository ip\n", STDERR_FILENO);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fputs("lpkg: connect failed\n", STDERR_FILENO);
        close(fd);
        return -1;
    }

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      path, ip);
    if (send(fd, req, (size_t)rl, 0) < 0) { close(fd); return -1; }

    /* Buffer the whole response, then split headers from the body. */
    unsigned long cap = 65536, len = 0;
    char *resp = malloc(cap);
    if (!resp) { close(fd); return -1; }
    ssize_t n;
    char tmp[2048];
    while ((n = recv(fd, tmp, sizeof(tmp), 0)) > 0) {
        if (len + (unsigned long)n + 1 > cap) {
            unsigned long ncap = cap * 2;
            char *nr = realloc(resp, ncap);
            if (!nr) { free(resp); close(fd); return -1; }
            resp = nr; cap = ncap;
        }
        memcpy(resp + len, tmp, (unsigned long)n);
        len += (unsigned long)n;
    }
    close(fd);
    resp[len] = '\0';

    /* Status line: "HTTP/1.x 200 ...". */
    if (strncmp(resp, "HTTP/", 5) != 0 || !strchr(resp, ' ') ||
        strncmp(strchr(resp, ' ') + 1, "200", 3) != 0) {
        fputs("lpkg: repository did not return 200 OK\n", STDERR_FILENO);
        free(resp);
        return -1;
    }

    /* Body begins after the blank line. */
    char *body = NULL;
    for (unsigned long i = 0; i + 3 < len; i++) {
        if (resp[i] == '\r' && resp[i+1] == '\n' && resp[i+2] == '\r' && resp[i+3] == '\n') {
            body = resp + i + 4;
            break;
        }
    }
    if (!body) { fputs("lpkg: malformed HTTP response\n", STDERR_FILENO); free(resp); return -1; }

    unsigned long blen = len - (unsigned long)(body - resp);
    int rc = install_file(outfile, body, blen, 0644);
    free(resp);
    return rc;
}

#define LPKG_TMP "/lpkg-download.lpkg"

/* `lpkg fetch <ip> <port> <path>`: download a package without installing. */
static int cmd_fetch(const char *ip, int port, const char *path, const char *out) {
    if (http_download(ip, port, path, out) != 0) return 1;
    printf("fetched %s -> %s\n", path, out);
    return 0;
}

/* `lpkg install-net <ip> <port> <path>`: download then install locally. */
static int cmd_install_net(const char *ip, int port, const char *path, int force) {
    if (http_download(ip, port, path, LPKG_TMP) != 0) return 1;
    int rc = cmd_install(LPKG_TMP, force);
    unlink(LPKG_TMP);
    return rc;
}

static int usage(void) {
    fputs("usage: lpkg <command> [args]\n"
          "  install <file.lpkg> [--force]   install a local package\n"
          "  remove  <name>                  remove an installed package\n"
          "  list                            list installed packages\n"
          "  info    <name>                  show package details\n"
          "  sync                            re-extract packages (post-reboot)\n"
          "  fetch   <ip> <port> <path> <out>   download a package over HTTP\n"
          "  install-net <ip> <port> <path>     download over HTTP then install\n",
          STDERR_FILENO);
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *cmd = argv[1];

    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) return usage();
        int force = (argc >= 4 && strcmp(argv[3], "--force") == 0);
        return cmd_install(argv[2], force);
    }
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) return usage();
        return cmd_remove(argv[2]);
    }
    if (strcmp(cmd, "list") == 0) return cmd_list();
    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) return usage();
        return cmd_info(argv[2]);
    }
    if (strcmp(cmd, "sync") == 0) return cmd_sync();
    if (strcmp(cmd, "fetch") == 0) {
        if (argc < 6) return usage();
        return cmd_fetch(argv[2], atoi(argv[3]), argv[4], argv[5]);
    }
    if (strcmp(cmd, "install-net") == 0) {
        if (argc < 5) return usage();
        int force = (argc >= 6 && strcmp(argv[5], "--force") == 0);
        return cmd_install_net(argv[2], atoi(argv[3]), argv[4], force);
    }
    return usage();
}
