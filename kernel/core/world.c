#include "vfs.h"
#include "serial.h"
#include "string.h"
#include "crypt_lite.h"
#include "world.h"

/* The /bin command set is materialized as real files by the ELF loader.  The
 * two setuid-root helpers are intentionally not in that table; they are written
 * here as owned, mode-bearing files so set-user-ID-on-exec has real VFS inodes
 * to act on. */
extern void elf_install_embedded_programs(void);
extern uint8_t _prog_su_start[], _prog_su_end[];
extern uint8_t _prog_passwd_start[], _prog_passwd_end[];

/* Nonzero when the persistent /var (ext4) volume failed to mount at boot, so
 * the system is running volatile with factory defaults (see adr/0019). */
static int g_rescue;

int world_is_rescue(void) { return g_rescue; }

/* -------------------------------------------------------------------------- *
 * Small VFS helpers
 * -------------------------------------------------------------------------- */

static void str_append(char *dst, size_t cap, size_t *off, const char *s) {
    while (*s && *off + 1 < cap) dst[(*off)++] = *s++;
    dst[*off] = '\0';
}

/* Substring test (no strstr in the freestanding string helpers). */
static int str_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (; *hay; hay++)
        if (strncmp(hay, needle, nl) == 0) return 1;
    return 0;
}

static void mkdir_if_absent(const char *path, uint32_t mode) {
    if (vfs_lookup_path(path)) return;
    vfs_mkdir(path, mode);
}

static int file_exists(const char *path) {
    struct vfs_file *f = vfs_open(path, O_RDONLY);
    if (!f) return 0;
    vfs_close(f);
    return 1;
}

/* Create a regular file with the given contents and ownership, persisting the
 * attributes so a factory seed survives a reboot on the ext4 volume. */
static void write_file(const char *path, const void *data, size_t len,
                       uint32_t mode, uint32_t uid, uint32_t gid) {
    struct vfs_file *f = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC);
    if (!f) {
        serial_printf(SERIAL_COM1, "[WORLD] failed to create %s\n", path);
        return;
    }
    if (len) vfs_write(f, data, len);
    if (f->inode) {
        f->inode->mode = mode;
        f->inode->uid  = uid;
        f->inode->gid  = gid;
        vfs_setattr(f->inode);
    }
    vfs_close(f);
}

/* -------------------------------------------------------------------------- *
 * Core namespace
 * -------------------------------------------------------------------------- */

void world_mount_core(void) {
    /* Volatile system root, rebuilt from the kernel image each boot. */
    vfs_mount("ramfs", NULL, "/");

    /* Ramfs read/write fixture consumed by init's userspace I/O self-test
     * (init opens /hello.txt; its absence aborts PID 1 before login comes up). */
    struct vfs_file *hello = vfs_open("/hello.txt", O_CREAT | O_WRONLY);
    if (hello) {
        vfs_write(hello, "Hello from ramfs!\n", 18);
        vfs_close(hello);
    }

    /* The persistent data volume.  If the ext4 image is absent or corrupt the
     * mount fails cleanly: flag rescue mode and continue volatile rather than
     * aborting the boot (the immutable /bin still reaches a login). */
    vfs_mkdir("/var", 0755);
    if (vfs_mount("ext4", "hdc", "/var") != 0) {
        g_rescue = 1;
        serial_print(SERIAL_COM1,
            "[WORLD] /var (ext4) mount failed - RESCUE MODE (no persistence)\n");
    }

    /* Synthetic introspection; always available regardless of /var. */
    mkdir_if_absent("/proc", 0555);
    if (vfs_mount("procfs", NULL, "/proc") != 0)
        serial_print(SERIAL_COM1, "[WORLD] procfs mount failed\n");

    /* Mountpoints used by the fstab-driven volumes below. */
    mkdir_if_absent("/mnt", 0755);
    mkdir_if_absent("/mnt/legacy", 0755);
}

/* -------------------------------------------------------------------------- *
 * Factory-default seeding (idempotent: only writes when the file is absent,
 * so a healthy data volume is never clobbered)
 * -------------------------------------------------------------------------- */

static void seed_factory_defaults(void) {
    if (!file_exists("/etc/passwd")) {
        static const char passwd_txt[] =
            "root:x:0:0:root:/home/root:/bin/sh\n"
            "user:x:1000:1000:user:/home/user:/bin/sh\n";
        write_file("/etc/passwd", passwd_txt, sizeof(passwd_txt) - 1,
                   S_IFREG | 0644, 0, 0);
    }

    if (!file_exists("/etc/shadow")) {
        char hroot[96], huser[96];
        crypt_lite("root", "rt", hroot, sizeof(hroot));
        crypt_lite("user", "us", huser, sizeof(huser));
        char shadow[256];
        size_t off = 0;
        str_append(shadow, sizeof(shadow), &off, "root:");
        str_append(shadow, sizeof(shadow), &off, hroot);
        str_append(shadow, sizeof(shadow), &off, ":\nuser:");
        str_append(shadow, sizeof(shadow), &off, huser);
        str_append(shadow, sizeof(shadow), &off, ":\n");
        write_file("/etc/shadow", shadow, off, S_IFREG | 0600, 0, 0);
    }

    if (!file_exists("/etc/group")) {
        static const char group_txt[] = "root:x:0:\nuser:x:1000:\n";
        write_file("/etc/group", group_txt, sizeof(group_txt) - 1,
                   S_IFREG | 0644, 0, 0);
    }

    if (!file_exists("/etc/hostname")) {
        static const char hostname[] = "lariat\n";
        write_file("/etc/hostname", hostname, sizeof(hostname) - 1,
                   S_IFREG | 0644, 0, 0);
    }

    if (!file_exists("/etc/profile")) {
        static const char profile[] =
            "export PATH=/usr/local/bin:/usr/bin:/bin\n";
        write_file("/etc/profile", profile, sizeof(profile) - 1,
                   S_IFREG | 0644, 0, 0);
    }

    /* Drives world_mount_fstab().  Harmless in rescue mode (the caller skips
     * fstab mounting when the data volume is absent). */
    if (!file_exists("/etc/fstab")) {
        static const char fstab[] =
            "hdc  /var         ext4    rw 0 0\n"
            "none /proc        procfs  rw 0 0\n"
            "hdb  /mnt/legacy  fat32   rw 0 0\n";
        write_file("/etc/fstab", fstab, sizeof(fstab) - 1,
                   S_IFREG | 0644, 0, 0);
    }
}

/* -------------------------------------------------------------------------- *
 * Multi-user world
 * -------------------------------------------------------------------------- */

void world_setup(void) {
    /* /bin is always ramfs (immutable, kernel-materialized) so a usable command
     * set exists even in rescue mode. */
    mkdir_if_absent("/bin", 0755);
    elf_install_embedded_programs();

    /* setuid-root helpers (not in the embedded table).  /bin is volatile, so
     * these are written fresh each boot. */
    write_file("/bin/su", _prog_su_start,
               (size_t)(_prog_su_end - _prog_su_start),
               S_IFREG | S_ISUID | 0755, 0, 0);
    write_file("/bin/passwd", _prog_passwd_start,
               (size_t)(_prog_passwd_end - _prog_passwd_start),
               S_IFREG | S_ISUID | 0755, 0, 0);

    if (!g_rescue) {
        /* macOS-style firmlinks: /etc, /home, /usr point into the ext4 volume,
         * so config, user data, and installed packages persist in place. */
        mkdir_if_absent("/var/etc", 0755);
        mkdir_if_absent("/var/home", 0755);
        mkdir_if_absent("/var/usr", 0755);
        vfs_symlink("/var/etc",  "/etc");
        vfs_symlink("/var/home", "/home");
        vfs_symlink("/var/usr",  "/usr");

        /* The musl dynamic loader is the lone PT_INTERP outside /usr; firmlink
         * it onto its persistent home (may dangle until libc-dev is installed). */
        mkdir_if_absent("/lib", 0755);
        vfs_symlink("/var/usr/lib/ld-musl-x86_64.so.1",
                    "/lib/ld-musl-x86_64.so.1");
    } else {
        /* No /var: /etc and /home are plain ramfs directories.  The /usr and
         * loader firmlinks are intentionally NOT created (their target volume is
         * absent), so dynamically-linked installed apps cannot run - but the
         * statically-linked /bin always reaches a login. */
        mkdir_if_absent("/etc", 0755);
        mkdir_if_absent("/home", 0755);
    }

    mkdir_if_absent("/home/root", 0755);
    mkdir_if_absent("/home/user", 0755);

    seed_factory_defaults();

    serial_print(SERIAL_COM1, g_rescue
        ? "[WORLD] rescue world initialised (ramfs-only, factory defaults)\n"
        : "[WORLD] multi-user world initialised (/etc,/home,/usr -> /var)\n");
}

/* -------------------------------------------------------------------------- *
 * fstab-driven mounts
 * -------------------------------------------------------------------------- */

/* Return the next whitespace-delimited token in *p, advancing *p past it.
 * Returns NULL when no more tokens remain. */
static char *next_token(char **p) {
    char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') { *p = s; return NULL; }
    char *start = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) { *s = '\0'; s++; }
    *p = s;
    return start;
}

/* Parse one fstab line and mount it if appropriate.  Returns 1 if a new mount
 * was added, 0 otherwise (skipped, noauto, already mounted, or malformed). */
static int fstab_mount_line(const char *line_raw) {
    char line[256];
    size_t l = 0;
    while (line_raw[l] && l + 1 < sizeof(line)) { line[l] = line_raw[l]; l++; }
    line[l] = '\0';

    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '#') return 0;

    char *dev  = next_token(&p);
    char *mp   = next_token(&p);
    char *fst  = next_token(&p);
    char *opts = next_token(&p);
    if (!dev || !mp || !fst) return 0;
    if (!opts) opts = "rw";

    if (str_contains(opts, "noauto")) return 0;
    if (vfs_is_mounted(mp)) return 0;          /* idempotent */

    if (!vfs_lookup_path(mp)) vfs_mkdir(mp, 0755);
    if (vfs_mount(fst, dev, mp) == 0) {
        serial_printf(SERIAL_COM1,
            "[WORLD] fstab: mounted %s on %s (%s)\n", dev, mp, fst);
        return 1;
    }
    serial_printf(SERIAL_COM1,
        "[WORLD] fstab: %s on %s (%s) FAILED\n", dev, mp, fst);
    return 0;
}

void world_mount_fstab(void) {
    struct vfs_file *f = vfs_open("/etc/fstab", O_RDONLY);
    if (!f) {
        serial_print(SERIAL_COM1, "[WORLD] no /etc/fstab (nothing to mount)\n");
        return;
    }

    char buf[1024];
    ssize_t total = 0;
    for (;;) {
        ssize_t r = vfs_read(f, buf + total, sizeof(buf) - 1 - total);
        if (r <= 0) break;
        total += r;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    vfs_close(f);
    buf[total] = '\0';

    int mounted = 0;
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        mounted += fstab_mount_line(line);
        if (!nl) break;
        line = nl + 1;
    }

    serial_printf(SERIAL_COM1, "[WORLD] fstab: %d additional mount(s)\n",
                  mounted);
}
