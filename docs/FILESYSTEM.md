# Project Lariat — Filesystem

This is the formal specification of Lariat's filesystem: its namespace, the VFS
contract, the persistence model, and the on-disk formats. It reflects the
decisions in `adr/0004` (persistent root) and `adr/0013` (unified namespace).

## 1. Philosophy

Lariat presents a **single, Unix-like namespace** rooted at `/`. Storage is
mounted at *purpose-named* locations; the backing device and filesystem type are
**metadata** (the mount table, `/etc/fstab`), never part of a path. There are no
DOS/Windows "drive letters."

The model is **macOS-style**: an *immutable system image* plus a *persistent
data volume*.

- The system root (`/`, `/bin`) is **ramfs**, rebuilt from the kernel image on
  every boot — always consistent, never corrupted by a crash. `/bin` is the
  immutable bootstrap command set (materialized from the kernel each boot).
- All persistent state lives on one **ext4 data volume** mounted at `/var`.
- `/etc`, `/home`, and the package install prefix **`/usr`** are **firmlinks**
  (symlinks) into `/var`, so configuration, user data, and installed
  apps/libraries persist in place while still appearing at their conventional
  Unix paths. Installed packages survive reboot with **no re-extraction**.

## 2. Namespace & mount layout

```
/                ramfs    immutable system root, rebuilt from the kernel each boot
├── bin/         ramfs    coreutils + system binaries — real files materialized
│                         from the kernel image at boot (ls/cp/stat see them)
├── lib/         ramfs    holds the loader firmlink ld-musl-x86_64.so.1 ► /var/usr/lib
├── etc   ─────► /var/etc     (firmlink)  persistent system configuration
├── home  ─────► /var/home    (firmlink)  persistent user home directories
├── usr   ─────► /var/usr     (firmlink)  persistent package install prefix
├── dev/         devfs    /dev/console, /dev/fb0, /dev/input, …
├── proc/        procfs   process + config + service introspection
├── var/         ext4     THE persistent data volume (read-write)
│   ├── etc/              passwd, shadow, group, hostname, lariat.conf, fstab, profile
│   ├── home/             root/, user/, <accounts>/
│   ├── usr/              bin/, lib/, libexec/, include/, … (installed packages)
│   └── lib/lpkg/         the package manager database (db/)
└── mnt/
    └── legacy/   fat32   legacy scratch volume (no longer authoritative)
```

The backing devices are IDE disks: `hdc` → ext4 (`/var`), `hdb` → FAT32
(`/mnt/legacy`). These appear only in the mount table and `/etc/fstab`, not in
paths.

`/bin` stays ramfs (immutable, kernel-materialized) so a usable command set and
PATH fallback always exist, even in rescue mode. `/usr`, by contrast, firmlinks
onto persistent ext4: `lpkg install` writes binaries under `/usr` once and they
survive every reboot. The musl dynamic loader is the lone `PT_INTERP` path
outside `/usr` (`/lib/ld-musl-x86_64.so.1`); `/lib` is a ramfs directory holding
just a firmlink to the loader's persistent home at `/var/usr/lib`.

### Boot mount sequence

Performed in `kmain()` (`kernel/kernel.c`), before `m10_setup()`:

```c
vfs_mount("ramfs", NULL, "/");           /* volatile system root      */
vfs_mkdir("/var", 0755);
vfs_mount("ext4",  "hdc", "/var");       /* persistent data volume    */
vfs_mkdir("/mnt", 0755);
vfs_mkdir("/mnt/legacy", 0755);
vfs_mount("fat32", "hdb", "/mnt/legacy");/* legacy scratch            */
vfs_mkdir("/proc", 0555);
vfs_mount("procfs", NULL, "/proc");
```

`m10_setup()` then creates the firmlinks and seeds (if absent) the persistent
config:

```c
vfs_mkdir("/var/etc", 0755);
vfs_mkdir("/var/home", 0755);
vfs_mkdir("/var/usr", 0755);
vfs_symlink("/var/etc",  "/etc");        /* firmlink */
vfs_symlink("/var/home", "/home");       /* firmlink */
vfs_symlink("/var/usr",  "/usr");        /* firmlink: persistent install prefix */
vfs_mkdir("/lib", 0755);
vfs_symlink("/var/usr/lib/ld-musl-x86_64.so.1",
            "/lib/ld-musl-x86_64.so.1"); /* loader firmlink (may dangle) */
```

Only these essentials are bootstrapped in `kmain` (they must be up before
`/etc/fstab` is readable). Everything else is **data-driven**: once the `/etc`
firmlink exists, `mount_fstab()` parses `/etc/fstab` and mounts the remaining
volumes (e.g. the FAT32 removable media at `/mnt/legacy`). Lines that are already
mounted or carry the `noauto` option are skipped, so it is idempotent.

### Mount table & `/proc/mounts`

The VFS records every successful mount in a small in-kernel table
(`device → mountpoint → fstype`). It backs `vfs_is_mounted()` (used for fstab
idempotency) and the synthetic **`/proc/mounts`** file:

```
hdc  /var         ext4    rw 0 0
none /proc        procfs  rw 0 0
hdb  /mnt/legacy  fat32   rw 0 0
```

### Rescue mode

If the `/var` (ext4) volume fails to mount at boot, the kernel sets a rescue
flag, logs it, and `m10_setup()` falls back to creating `/etc` and `/home` as
plain ramfs directories seeded with factory defaults. The `/usr` and loader
firmlinks are **not** created in rescue mode (their target volume is absent), so
dynamically-linked installed apps cannot run — but the immutable, statically
linked `/bin` command set always works. The system still boots to a usable
login; state simply does not persist until the data volume is healthy.

## 3. VFS contract (`include/vfs.h`, `kernel/vfs.c`)

The VFS is the abstraction every filesystem implements.

### Objects

| Object | Role |
|--------|------|
| `vfs_superblock` | a mounted filesystem instance (`fs_type`, `root` dentry, private) |
| `vfs_inode` | a file/dir/symlink: `mode`, `size`, `nlink`, `uid`/`gid`, `i_ops`, `f_ops` |
| `vfs_dentry` | name → inode, with `parent`, children, and `mount` (mounted sb, or NULL) |
| `vfs_file` | an open handle: `dentry`, `inode`, `pos`, `flags`, `ref_count` |

### `vfs_inode_ops` (directory/namespace operations)

`lookup`, `create`, `mkdir`, `unlink`, `rmdir`, `readdir`, **`symlink`**,
**`readlink`**, **`setattr`**. A filesystem provides what it supports;
unsupported ops are `NULL` and the VFS returns an error.

`setattr` flushes an inode's `mode`/`uid`/`gid` to the backing store, so a
`chmod`/`chown` (or creator ownership on `mkdir`/`open(O_CREAT)`) survives the
next lookup and a remount. The VFS wrapper `vfs_setattr()` no-ops for in-memory
filesystems (ramfs), where the in-core inode is already the source of truth.

### `vfs_file_ops` (open-file operations)

`read`, `write`, `close`, `lseek`, `poll`, `ioctl`, `truncate`, `mmap`.

### Path resolution

`vfs_lookup_path()` resolves an absolute path by walking components from the
root (`vfs_walk`):

1. `.` is skipped; `..` ascends via `dentry->parent`. Because disk filesystems
   (ext4) mint a fresh dentry per `lookup` and cannot know the parent dentry,
   the walk anchors each resolved child to the directory it was looked up in
   (`next->parent = current`), so a later `..` ascends to the *real* parent.
   This is what lets `..`-relative paths such as gcc's internal
   `/usr/bin/../libexec/gcc/.../cc1` resolve across the ext4 volume.
2. **Mount crossing:** before resolving inside a directory and on the final
   dentry, while `dentry->mount` is set, the walk steps into that mounted
   superblock's root. This is how `/var` resolves into the ext4 volume.
3. **Component lookup:** via `i_ops->lookup` (or the cached dentry children for
   ramfs).
4. **Symlink following:** if a resolved component is `S_IFLNK`, its target is
   read (`i_ops->readlink`) and resolution continues from there — absolute
   targets restart at `/`, relative targets continue from the symlink's
   directory. Recursion is bounded by `VFS_SYMLINK_MAX_DEPTH` (8) to stop loops.

`vfs_lookup_parent()` resolves all but the last component (following mounts and
symlinks), so creating `/etc/passwd` correctly targets the ext4 directory behind
the `/etc` firmlink.

### Symbolic links

- `vfs_symlink(target, linkpath)` creates a link; `vfs_readlink(path, …)` reads
  a link's target *without following it*.
- Syscalls `symlink(2)` / `readlink(2)` are wired to these (`cpu/syscall.c`).
- ramfs stores a symlink's target in the inode's data; ext4 symlink inodes are
  read-only at present (the firmlinks live in ramfs).

## 4. Filesystems

### ramfs (`kernel/fs/ramfs.c`) — system root

In-memory directories, regular files, and symlinks. Backs `/`, `/bin`, `/dev`
(via devfs), and the `/etc` and `/home` firmlink nodes. Volatile by design.

### ext4 (`kernel/fs/ext4.c`) — persistent data volume

A writable subset of ext4, validated `e2fsck -fn` clean. Supported on-disk
features:

- Superblock, block-group descriptors, block/inode **bitmaps** (allocate +
  free), the inode table.
- **Extent-mapped** inodes with full **extent-tree** writes: files start with
  the inline (depth-0) list in the inode (4 records) and grow into a real
  on-disk tree (index + leaf blocks, arbitrary depth) when it fills, so
  large/fragmented files (gcc's 43 MB `cc1`) write correctly. Block allocation
  hands out **contiguous runs** to keep the extent count low, and out-of-order
  (back-seek) writes — e.g. a linker patching its output — are inserted in
  **sorted** logical-block order so the on-disk tree stays `e2fsck`-clean.
  Truncate/unlink free both data blocks and tree (index/leaf) metadata.
- Directory entries (`ext4_dir_entry_2`): idempotent insert (`create`/`mkdir`
  reject duplicates via `ext4_dir_find`) and remove.
- Inode writeback, file **truncate**, link-count maintenance, and proper
  **deleted-inode stamping** (`i_links_count = 0`, real `i_dtime`, extents
  cleared) so `e2fsck` sees no orphans.
- **Inode metadata persistence** (`ext4_setattr`): the real on-disk `i_mode`
  (type + permission bits) and `i_uid`/`i_gid` are loaded on lookup and written
  back on `chmod`/`chown`/creator-ownership changes. This is what lets an
  ordinary user own `~/.local/bin` on `/var` and install an executable there
  that survives a reboot.

Writes are persisted to the host image via QEMU `cache=writethrough`.

### fat32 (`kernel/fs/fat32.c`) — legacy

Read-write FAT32 at `/mnt/legacy`. No longer the source of truth; slated for
retirement (roadmap N3).

### procfs (`kernel/fs/procfs.c`) — introspection

`/proc` exposes process, configuration, and service state (`adr/0009`).

## 5. Persistence model

| Path | Backing | Persists? | Notes |
|------|---------|-----------|-------|
| `/`, `/bin` | ramfs | No | rebuilt from the kernel image each boot |
| `/lib` | ramfs | No | holds only the loader firmlink → `/var/usr/lib` |
| `/etc` | ext4 via firmlink → `/var/etc` | **Yes** | passwd/shadow/group/hostname/conf |
| `/home` | ext4 via firmlink → `/var/home` | **Yes** | per-user home directories |
| `/usr` | ext4 via firmlink → `/var/usr` | **Yes** | installed packages (bin/lib/libexec/include) |
| `/var` | ext4 (`hdc`) | **Yes** | the data volume root |
| `/var/lib/lpkg` | ext4 | **Yes** | package database (`db/`) |
| `/mnt/legacy` | fat32 (`hdb`) | Yes | legacy scratch, not authoritative |
| `/proc`, `/dev` | procfs/devfs | No | synthetic |

Because `/etc`, `/home`, and `/usr` are firmlinks onto ext4, edits (via
`useradd`, `passwd`, the Settings app, an editor, …) and package installs land
on disk **in place**. There is no copy-in at boot and no `etc_sync` mirroring —
`m10_setup` only *seeds factory defaults when absent*, and `etc_sync()` is a
retained no-op.

`lpkg install` writes package payloads directly under the persistent `/usr`
prefix, so they survive reboot with no re-extraction. Boot only mounts `/var`
and re-creates the firmlinks (a few milliseconds), regardless of how much is
installed. The DB under `/var/lib/lpkg` records the install for `list`/`info`/
`remove`; `lpkg sync` is now a no-op (the old boot-time re-extract is retired).

## 6. Recovery & integrity

- The ext4 volume is consistent after clean and crash exits given
  write-through caching; verified `e2fsck -fn` clean across create/write/
  mkdir/unlink/rmdir cycles and across reboots.
- The system tree is immutable and regenerated, so a corrupted `/bin` cannot
  brick the system — only the data volume carries state.
- A missing/unmountable `/var` drops to **ramfs-only rescue mode** (factory
  defaults seeded in ramfs, no persistence) rather than failing to boot.

## 7. Executable resolution

Lariat resolves commands the way Linux/macOS do — by **searching `$PATH` for a
real file**, not via a hardcoded in-kernel command table:

- At boot, `world_setup()` materializes every kernel-embedded program as a real
  `0755` file under `/bin` (the in-kernel `g_embedded` table is now only a
  last-resort exec fallback). So `/bin` is an ordinary, inspectable, extensible
  directory: `ls`, `cp`, and `stat` all see real inodes, and `execve` finds them
  by plain path lookup.
- The shell (`sh`) splits `$PATH` on `:` and tries each directory; a name
  containing `/` is treated as a literal path.
- `login(8)` builds `$PATH` per account: root gets the `*sbin` + `*bin` system
  trees; an ordinary user gets `~/.local/bin` and `~/bin` **prepended** to the
  system `*bin` trees. Combined with ext4 metadata persistence and per-user
  ownership of `$HOME`, this gives each user a private, persistent place to
  install commands without root.
- `/etc/profile` seeds the system-wide default (`export
  PATH=/usr/local/bin:/usr/bin:/bin`); `login` augments it per user.

## 8. Limitations & future work

- Essential mounts are bootstrapped in `kmain`; the rest are `fstab`-driven via
  `mount_fstab()`, surfaced in `/proc/mounts`.
- No **bind mounts** and no subdirectory mounts; firmlinks use symlinks.
- ext4 writes use a full extent tree (inline depth-0 list growing into on-disk
  index/leaf blocks), so large and fragmented files write correctly; there is no
  journaling (the volume is crash-consistent via write-through, not via the ext4
  journal).
- FAT32 remains mounted as legacy and is pending removal (N3).
- See `adr/0013` for the longer-term option of making ext4 the true root `/`.
