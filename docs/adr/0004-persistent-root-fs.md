# ADR-0004: Persistent writable root filesystem

- Status: Accepted
- Date: 2026-05-30

## Context

The root filesystem is volatile ramfs: `/etc` (passwd/shadow), `/tmp`, and the
setuid helpers are re-created on every boot by `m10_setup` in `kernel/kernel.c`.
`/disk` is writable FAT32; `/ext4` is mounted read-only. Settings, accounts, and
installed packages cannot survive a reboot. Nearly every later goal (package
database, user-editable config, account changes) needs persistence.

## Decision

Provide a persistent writable root. Near term, make ext4 writable
(`ext4_file_write`) or adopt the FAT32 `/disk` as the system partition for state
that must persist (`/etc`, `/usr`, `/var`). Add a `mkfs`/image-build script
(none exists today; `*.img` are gitignored). Change boot seeding so defaults are
written only when missing, rather than unconditionally overwriting persisted
state.

## Consequences

- `m10_setup` becomes "seed if absent" instead of "always seed".
- Requires an image-build tool committed to the repo (scripts, not images).
- Unblocks the package database (ADR-0003), persisted settings (Phase 4), and
  account administration.

## Update (2026-05-30): ext4 is the persistent root

The in-kernel ext4 driver (`kernel/fs/ext4.c`) is now fully read-write:
extent append, block/inode bitmap allocation and free, directory entry
insert/remove (with idempotent `create`/`mkdir`), inode writeback, truncate,
and superblock/group-descriptor count maintenance. Deleted inodes are stamped
(`i_links_count = 0`, real `i_dtime`) so `e2fsck -fn` reports the volume clean
after create/write/mkdir/unlink/rmdir cycles and across reboots.

Persistence was therefore migrated off FAT32 onto the ext4 volume:

- System state moved from `/disk/etc` to `/ext4/etc`
  (`LARIAT_PERSIST_*` and the config table in `kernel/kernel.c`). ext4 supports
  long names, so the FAT32 8.3 workaround (`larcfg`) is gone — the persistent
  copy is now `/ext4/etc/lariat.conf`.
- Userspace account/group/hostname sync (`etc_sync` in `userspace/libc/pwd.c`)
  writes to `/ext4/etc`.
- The package database (`userspace/lpkg.c` `DB_ROOT`/`DB_DIR`) and package
  delivery live under `/ext4/var/lib/lpkg` and `/ext4`.
- The reliable IDE secondary channel (ADR/Makefile: `-device ide-hd,bus=ide.1`,
  `cache=writethrough`, longer ATA soft-reset/settle) makes `/ext4` mount on
  every boot.

FAT32 `/disk` is still mounted as a legacy scratch volume but is no longer the
source of truth; it can be dropped entirely in a follow-up.

Validation: two consecutive boots on the same image show factory seeding on
boot 1 ("accounts seeded and persisted to /ext4/etc") and restore on boot 2
("accounts restored from persistent /ext4/etc"), with `e2fsck -fn` clean
(rc=0) and exactly one `/etc` entry after both boots.
