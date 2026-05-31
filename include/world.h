#ifndef WORLD_H
#define WORLD_H

/*
 * world - boot-time filesystem world setup.
 *
 * Brings up the Unix-like namespace and the multi-user world at boot, kept out
 * of kmain so kernel.c stays close to "just the boot sequence."  See
 * docs/FILESYSTEM.md and adr/0004 (persistent root) / adr/0013 (unified
 * namespace) / adr/0014 (PATH-based executable resolution).
 */

/* Mount the core namespace: ramfs `/`, ext4 `/var` (the persistent data
 * volume), and procfs `/proc`.  If `/var` fails to mount, the rescue flag is
 * set (see world_is_rescue) and the system continues in a volatile world.
 * Must run after the filesystem drivers are initialised. */
void world_mount_core(void);

/* Lay down the multi-user world (formerly "m10"): materialize /bin from the
 * kernel image, firmlink /etc and /home into /var, and seed factory defaults
 * (accounts, group, hostname, fstab, profile, setuid helpers) when absent.
 * Idempotent: seeds if absent rather than clobbering persisted state. */
void world_setup(void);

/* Data-driven mounts: after the /etc firmlink is up, parse /etc/fstab and bring
 * up the remaining volumes (skipping already-mounted and `noauto` entries). */
void world_mount_fstab(void);

/* Nonzero if the persistent /var (ext4) volume failed to mount at boot and the
 * system is running in ramfs-only rescue mode. */
int world_is_rescue(void);

#endif /* WORLD_H */
