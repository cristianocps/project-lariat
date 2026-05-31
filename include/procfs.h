#ifndef PROCFS_H
#define PROCFS_H

/* Minimal synthetic /proc filesystem exposing kernel tunables (Phase 4).
 * See kernel/fs/procfs.c and docs/adr/0009-procfs-and-config.md. */

void procfs_init(void);

/* Live system hostname (nodename for uname).  Defaults to "lariat"; settable
 * via /proc/sys/kernel/hostname and /proc/hostname. */
const char *sys_hostname(void);
void        sys_set_hostname(const char *name);

#endif /* PROCFS_H */
