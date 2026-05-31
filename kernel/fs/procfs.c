#include "vfs.h"
#include "procfs.h"
#include "kapi.h"
#include "serial.h"
#include "pmm.h"
#include "timer.h"
#include "net.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * procfs - a tiny read-mostly synthetic filesystem for kernel tunables.
 *
 * Files are backed by a generator (produces current content on read) and an
 * optional setter (consumes a write).  Directories reuse the VFS dentry tree
 * built at mount time.  The fs is otherwise immutable (no create/mkdir).
 * See docs/adr/0009-procfs-and-config.md.
 * -------------------------------------------------------------------------- */

/* Live hostname tunable. */
static char g_hostname[65] = "lariat";
const char *sys_hostname(void) { return g_hostname; }
void sys_set_hostname(const char *name) {
    if (!name) return;
    size_t i = 0;
    for (; name[i] && name[i] != '\n' && i < sizeof(g_hostname) - 1; i++)
        g_hostname[i] = name[i];
    g_hostname[i] = '\0';
}

/* ---- small formatting helpers ------------------------------------------- */
static int put_str(char *out, int max, int off, const char *s) {
    for (int i = 0; s[i] && off < max; i++) out[off++] = s[i];
    return off;
}
static int put_u64(char *out, int max, int off, uint64_t v) {
    char tmp[24];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = '0' + (int)(v % 10); v /= 10; }
    while (n > 0 && off < max) out[off++] = tmp[--n];
    return off;
}

/* ---- generators ---------------------------------------------------------- */
typedef int (*proc_gen_t)(char *out, int max);
typedef int (*proc_set_t)(const char *in, int n);

typedef struct {
    proc_gen_t gen;
    proc_set_t set;
} proc_entry_t;

static int gen_version(char *o, int m) {
    int n = 0;
    n = put_str(o, m, n, "Lariat 0.2 Project Lariat SMP x86_64\n");
    return n;
}
static int gen_hostname(char *o, int m) {
    int n = put_str(o, m, 0, g_hostname);
    if (n < m) o[n++] = '\n';
    return n;
}
static int set_hostname(const char *in, int n) { (void)n; sys_set_hostname(in); return 0; }

static int gen_uptime(char *o, int m) {
    uint64_t ticks = timer_get_ticks();
    uint64_t secs = ticks / TIMER_HZ;
    uint64_t cs = (ticks % TIMER_HZ) * 100 / TIMER_HZ;
    int n = put_u64(o, m, 0, secs);
    if (n < m) o[n++] = '.';
    if (cs < 10 && n < m) o[n++] = '0';
    n = put_u64(o, m, n, cs);
    if (n < m) o[n++] = '\n';
    return n;
}
static int gen_meminfo(char *o, int m) {
    uint64_t total = pmm_total_pages();
    uint64_t freep = pmm_get_free_count();
    int n = 0;
    n = put_str(o, m, n, "MemTotal: ");
    n = put_u64(o, m, n, total * 4);
    n = put_str(o, m, n, " kB\nMemFree: ");
    n = put_u64(o, m, n, freep * 4);
    n = put_str(o, m, n, " kB\n");
    return n;
}
static int gen_timer_hz(char *o, int m) {
    int n = put_u64(o, m, 0, TIMER_HZ);
    if (n < m) o[n++] = '\n';
    return n;
}
static int gen_mounts(char *o, int m) {
    int n = 0;
    int count = vfs_mounts_count();
    for (int i = 0; i < count; i++) {
        const struct vfs_mount_rec *r = vfs_mounts_get(i);
        if (!r) continue;
        /* device mountpoint fstype options dump pass  (fstab/mtab style) */
        n = put_str(o, m, n, r->dev);
        if (n < m) o[n++] = ' ';
        n = put_str(o, m, n, r->path);
        if (n < m) o[n++] = ' ';
        n = put_str(o, m, n, r->fstype);
        n = put_str(o, m, n, " rw 0 0\n");
    }
    return n;
}
static int gen_netinfo(char *o, int m) {
    netif_t *nif = netif_default();
    int n = 0;
    if (!nif) return put_str(o, m, 0, "no interface\n");
    n = put_str(o, m, n, nif->name);
    n = put_str(o, m, n, " ip ");
    uint32_t ip = nif->ip;   /* network byte order */
    n = put_u64(o, m, n, (ip) & 0xff);       if (n < m) o[n++] = '.';
    n = put_u64(o, m, n, (ip >> 8) & 0xff);  if (n < m) o[n++] = '.';
    n = put_u64(o, m, n, (ip >> 16) & 0xff); if (n < m) o[n++] = '.';
    n = put_u64(o, m, n, (ip >> 24) & 0xff);
    if (n < m) o[n++] = '\n';
    return n;
}

/* ---- filesystem plumbing ------------------------------------------------- */
static struct vfs_file_ops  procfs_file_ops;
static struct vfs_inode_ops procfs_inode_ops;

typedef struct { uint32_t next_ino; } procfs_sb_t;

static struct vfs_inode *proc_mkinode(struct vfs_superblock *sb, uint32_t mode,
                                      proc_entry_t *e) {
    procfs_sb_t *psb = (procfs_sb_t *)sb->private_data;
    struct vfs_inode *inode = kzalloc(sizeof(struct vfs_inode));
    if (!inode) return NULL;
    inode->inode_no = psb->next_ino++;
    inode->mode = mode;
    inode->nlink = 1;
    inode->sb = sb;
    inode->i_ops = &procfs_inode_ops;
    inode->f_ops = S_ISDIR(mode) ? NULL : &procfs_file_ops;
    inode->private_data = e;
    return inode;
}

static struct vfs_dentry *proc_add(struct vfs_superblock *sb, struct vfs_dentry *parent,
                                   const char *name, uint32_t mode, proc_entry_t *e) {
    struct vfs_inode *inode = proc_mkinode(sb, mode, e);
    if (!inode) return NULL;
    struct vfs_dentry *d = vfs_dentry_create(name, inode, parent);
    if (!d) return NULL;
    vfs_dentry_add_child(parent, d);
    return d;
}

/* read: regenerate content and serve the [pos, pos+count) slice. */
static ssize_t procfs_read(struct vfs_file *file, void *buf, size_t count) {
    if (!file || !file->inode) return -1;
    proc_entry_t *e = (proc_entry_t *)file->inode->private_data;
    if (!e || !e->gen) return 0;
    char tmp[512];
    int len = e->gen(tmp, (int)sizeof(tmp));
    if (file->pos >= len) return 0;
    size_t avail = (size_t)(len - file->pos);
    if (count > avail) count = avail;
    memcpy(buf, tmp + file->pos, count);
    file->pos += (off_t)count;
    return (ssize_t)count;
}

static ssize_t procfs_write(struct vfs_file *file, const void *buf, size_t count) {
    if (!file || !file->inode) return -1;
    proc_entry_t *e = (proc_entry_t *)file->inode->private_data;
    if (!e || !e->set) return -1;
    char tmp[128];
    size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
    memcpy(tmp, buf, n);
    tmp[n] = '\0';
    if (e->set(tmp, (int)n) < 0) return -1;
    return (ssize_t)count;
}

static int procfs_close(struct vfs_file *file) { (void)file; return 0; }
static off_t procfs_lseek(struct vfs_file *file, off_t off, int whence) {
    if (!file) return -1;
    if (whence == 0) file->pos = off;
    else if (whence == 1) file->pos += off;
    else return -1;
    if (file->pos < 0) file->pos = 0;
    return file->pos;
}

static struct vfs_file_ops procfs_file_ops = {
    .read = procfs_read, .write = procfs_write,
    .close = procfs_close, .lseek = procfs_lseek,
};

/* Directory ops mirror ramfs's dentry-tree walk (read-only fs). */
static struct vfs_dentry *proc_dentry_for(struct vfs_dentry *d, struct vfs_inode *inode) {
    if (!d) return NULL;
    if (d->inode == inode) return d;
    for (struct vfs_dentry *c = d->child_list; c; c = c->next_sibling) {
        struct vfs_dentry *r = proc_dentry_for(c, inode);
        if (r) return r;
    }
    return NULL;
}
static struct vfs_dentry *proc_parent(struct vfs_inode *dir) {
    if (!dir || !dir->sb || !dir->sb->root) return NULL;
    return proc_dentry_for(dir->sb->root, dir);
}
static struct vfs_dentry *procfs_lookup(struct vfs_inode *dir, const char *name) {
    struct vfs_dentry *p = proc_parent(dir);
    return p ? vfs_dentry_find_child(p, name) : NULL;
}
static int procfs_readonly(void) { return -1; }
static int procfs_create(struct vfs_inode *d, const char *n, uint32_t m) { (void)d;(void)n;(void)m; return procfs_readonly(); }
static int procfs_mkdir(struct vfs_inode *d, const char *n, uint32_t m) { (void)d;(void)n;(void)m; return procfs_readonly(); }
static int procfs_unlink(struct vfs_inode *d, const char *n) { (void)d;(void)n; return procfs_readonly(); }
static int procfs_rmdir(struct vfs_inode *d, const char *n) { (void)d;(void)n; return procfs_readonly(); }
static int procfs_readdir(struct vfs_inode *dir, int index, struct vfs_dir_entry *entry) {
    struct vfs_dentry *p = proc_parent(dir);
    if (!p) return -1;
    struct vfs_dentry *c = p->child_list;
    int idx = 0;
    while (c) {
        if (idx == index) {
            size_t i = 0;
            while (c->name[i] && i < sizeof(entry->name) - 1) { entry->name[i] = c->name[i]; i++; }
            entry->name[i] = '\0';
            entry->inode_no = c->inode ? c->inode->inode_no : 0;
            entry->mode = c->inode ? c->inode->mode : 0;
            return 1;
        }
        idx++;
        c = c->next_sibling;
    }
    return 0;
}

static struct vfs_inode_ops procfs_inode_ops = {
    .lookup = procfs_lookup, .create = procfs_create, .mkdir = procfs_mkdir,
    .unlink = procfs_unlink, .rmdir = procfs_rmdir, .readdir = procfs_readdir,
};

/* Static entry tables (no per-inode allocation needed; entries are const). */
static proc_entry_t e_version  = { gen_version,  NULL };
static proc_entry_t e_hostname = { gen_hostname, set_hostname };
static proc_entry_t e_uptime   = { gen_uptime,   NULL };
static proc_entry_t e_meminfo  = { gen_meminfo,  NULL };
static proc_entry_t e_timerhz  = { gen_timer_hz, NULL };
static proc_entry_t e_netinfo  = { gen_netinfo,  NULL };
static proc_entry_t e_mounts   = { gen_mounts,   NULL };

static struct vfs_superblock *procfs_mount(const char *dev) {
    (void)dev;
    procfs_sb_t *psb = kzalloc(sizeof(procfs_sb_t));
    if (!psb) return NULL;
    psb->next_ino = 1;
    struct vfs_superblock *sb = kzalloc(sizeof(struct vfs_superblock));
    if (!sb) { kfree(psb); return NULL; }
    sb->private_data = psb;

    struct vfs_inode *root = proc_mkinode(sb, S_IFDIR | 0555, NULL);
    sb->root = vfs_dentry_create("/", root, NULL);

    struct vfs_dentry *r = sb->root;
    proc_add(sb, r, "version",  S_IFREG | 0444, &e_version);
    proc_add(sb, r, "hostname", S_IFREG | 0644, &e_hostname);
    proc_add(sb, r, "uptime",   S_IFREG | 0444, &e_uptime);
    proc_add(sb, r, "meminfo",  S_IFREG | 0444, &e_meminfo);
    proc_add(sb, r, "mounts",   S_IFREG | 0444, &e_mounts);

    struct vfs_dentry *net = proc_add(sb, r, "net", S_IFDIR | 0555, NULL);
    if (net) proc_add(sb, net, "info", S_IFREG | 0444, &e_netinfo);

    struct vfs_dentry *sys = proc_add(sb, r, "sys", S_IFDIR | 0555, NULL);
    struct vfs_dentry *ker = sys ? proc_add(sb, sys, "kernel", S_IFDIR | 0555, NULL) : NULL;
    if (ker) {
        proc_add(sb, ker, "hostname", S_IFREG | 0644, &e_hostname);
        proc_add(sb, ker, "timer_hz", S_IFREG | 0444, &e_timerhz);
    }
    return sb;
}

static int procfs_unmount(struct vfs_superblock *sb) {
    if (!sb) return -1;
    kfree(sb->private_data);
    kfree(sb);
    return 0;
}

static struct vfs_fs_type procfs_fs_type = {
    .name = "procfs", .mount = procfs_mount, .unmount = procfs_unmount, .next = NULL,
};

void procfs_init(void) {
    vfs_register_fs(&procfs_fs_type);
    serial_print(SERIAL_COM1, "[PROCFS] registered\n");
}
