# ADR-0013: Unified filesystem namespace (drop "drive letter" mounts)

- Status: Accepted (Option B)
- Date: 2026-05-30

## Context

Lariat currently mounts storage at points named after the **filesystem type /
backing device**:

```
/        ramfs   (volatile system root, rebuilt from the kernel image each boot)
/disk    fat32   (legacy scratch volume)
/ext4    ext4    (persistent state, package DB, package delivery)
/proc    procfs
```

This is the DOS/Windows "drive letter" model (`C:`, `D:`): the path leaks the
storage backend. Applications hardcode `/ext4/...`; if the persistent volume
were ever a different filesystem, every path would churn. It is the opposite of
how Unix presents storage.

On **Linux**, there is a single rooted tree following the Filesystem Hierarchy
Standard (FHS). The root `/` is itself a real persistent filesystem; other
volumes mount at *purpose-named* locations (`/home`, `/var`, `/boot`,
`/mnt/<x>`, `/media/<label>`). The backing device and fs type are **metadata**
(recorded in `/etc/fstab`, shown by `mount` / `df`), never part of the path.

On **macOS**, the system volume is a read-only, always-consistent image; a
separate writable *data* volume is firmlinked in, and removable/extra volumes
appear under `/Volumes/<name>`. The OS image is immutable; only data persists.

Mounts today are hardcoded in `kmain()` (`kernel/kernel.c`), and there is no
runtime `fstab` parser (the seeded `/etc/fstab` is documentation only).
`m10_setup` copies persistent state from the storage volume into the ramfs
`/etc` on boot, and userspace mirrors edits back via `etc_sync` — a dance that
exists only because the persistent store is a separate "drive."

## Decision (proposed — pick one direction)

Adopt a single Unix-like hierarchy. The fs type disappears from paths; storage
is mounted at purpose-named locations and described by a mount table. Two
end-state shapes are on the table; the difference is **what backs the root**.

### Option A — Persistent ext4 *is* the root `/` (Linux-style)

Mount the ext4 volume at `/` at boot. `/etc`, `/usr`, `/var`, `/home` become
real, persistent directories. `tmpfs`/ramfs backs only `/tmp` and `/run`;
`procfs` at `/proc`; devfs at `/dev`.

- **Pros:** the most Linux-like; the cleanest end state. Eliminates the
  `m10_setup` copy-in and `etc_sync` mirroring entirely — persistent state lives
  in place. One filesystem to reason about.
- **Cons:** the largest change and highest risk — boot now depends on mounting a
  real disk before `init`, so it needs a rescue/fallback path (boot to a
  ramfs-only shell if the root volume is missing/corrupt). The kernel currently
  writes `/bin` from its embedded image each boot; that either moves to a
  one-time install onto the root, or keeps happening on the writable root.

### Option B — Immutable ramfs system root + persistent data volume (macOS-style)

Keep `/` as ramfs, rebuilt from the kernel image each boot (an *immutable,
always-consistent system image* — a genuine feature Lariat already has). Mount
the ext4 persistent volume at a purpose-named location for variable/persistent
state, and make the persistent subtrees appear at their FHS paths:

```
/            ramfs    (immutable system image, rebuilt each boot)
/var         ext4     (persistent: /var/lib/lpkg, /var/db, system config)
/etc      -> persistent  (bind-mount or symlink into the data volume)
/home     -> persistent  (likewise)
/mnt/<x>             (extra/removable volumes; /media/<label> alias optional)
/proc        procfs
/dev         devfs
```

- **Pros:** preserves the resilient "stateless OS, persistent data" model and
  matches the user's macOS leaning. Removes the drive-letter wart without making
  the root depend on a disk. Lower risk than Option A.
- **Cons:** requires VFS support for **bind mounts or symlinks** so `/etc` and
  `/home` resolve into the data volume; the persistent-store relocation
  (`/ext4/...` → `/var/...`, `/etc`) still touches the same call sites as the
  recent FAT32→ext4 migration.

### Option C — Minimal relocation (stepping stone)

Keep the ramfs-root / separate-store split exactly as today, but **rename the
persistent mount from `/ext4` to a purpose name** (`/var` or `/mnt/data`) and
mount removable FAT32 under `/mnt/<x>`. Lowest effort; removes the fs-type name
from paths but keeps the `/etc` mirroring. Useful as phase 1 of Option A or B.

### Cross-cutting (all options)

- Introduce a **mount table** describing `device → mountpoint → fstype → flags`,
  and a runtime `fstab` parser so mounts are data, not hardcoded in `kmain`.
- `mount` / `df` (or a `/proc/mounts`) surface the device + fs type as metadata.
- Removable/extra volumes mount under `/mnt` (Linux) and/or `/Volumes` (mac).

## Recommendation

Target **Option B** (macOS-style immutable system + persistent data volume),
reached incrementally:

1. **C as step 1:** relocate the persistent mount `/ext4` → `/var`; mount FAT32
   (if kept) under `/mnt/legacy`. Update the recent persistence call sites
   (`LARIAT_PERSIST_*`, `etc_sync`, `lpkg` `DB_ROOT/DB_DIR`).
2. Add **symlink (then bind-mount) support** in the VFS so `/etc` and `/home`
   resolve into the data volume; retire the `m10_setup` copy-in / `etc_sync`
   mirroring for those subtrees.
3. Add the **mount table + `fstab` parser**; move the mount sequence out of
   `kmain` into data.
4. Provide a **rescue path** (ramfs-only shell) when the data volume is absent.

Rationale: Lariat already regenerates the system tree from the kernel image each
boot, which *is* the macOS immutable-system model. Option B keeps that strength
(and its resilience), removes the drive-letter naming, and avoids making the
root depend on a disk. Option A remains the longer-term end state if a fully
persistent root is desired; the steps above also set it up.

## Implementation status

**Option B is implemented and validated** (see `FILESYSTEM.md` for the formal
spec). Delivered in two phases:

- **Phase 1 (step C):** the persistent ext4 volume mounts at `/var` (was
  `/ext4`) and FAT32 at `/mnt/legacy` (was `/disk`). All persistence call sites
  (`m10_setup`, `etc_sync`, `lpkg` DB, init self-tests) moved to `/var`.
- **Phase 2 (firmlinks):** the VFS gained **symbolic links** — `vfs_inode_ops`
  `symlink`/`readlink`, symlink following in `vfs_walk` (depth-limited),
  ramfs symlink inodes, and wired `sys_symlink`/`sys_readlink`. `m10_setup` now
  creates `/etc → /var/etc` and `/home → /var/home` firmlinks, so writes to
  `/etc` and `/home` land on persistent ext4 in place. The boot-time copy-in
  and the userspace `etc_sync` mirroring were **retired** (`etc_sync` is now a
  no-op). Verified across two reboots (seed → "accounts present on persistent
  /etc") with `e2fsck -fn` clean.

- **N6 (mount table + fstab + rescue):** a kernel mount table backs
  `/proc/mounts`; only ramfs `/`, ext4 `/var`, and procfs `/proc` are
  bootstrapped in `kmain`, with the rest mounted from `/etc/fstab` via
  `mount_fstab()` (idempotent; honours `noauto`). A failed `/var` mount drops to
  a ramfs-only rescue mode rather than failing to boot.

The only remaining end-state item is Option A (ext4 as the true root `/`), which
is optional; the current immutable-system + data-volume design is the target.

## Consequences

- `ARCHITECTURE.md` §Filesystems and the seeded `/etc/fstab` are rewritten; the
  formal FS document (roadmap N2) is written once the shape is chosen.
- Requires new VFS capability (symlink and/or bind mount) for Option B.
- The FAT32 retirement (roadmap N3) becomes natural: `/disk` either disappears
  or becomes an optional read-only `/mnt` mount.
- Existing absolute paths (`/ext4/...`) used by tools/tests must be updated;
  this is the same surface touched by the FAT32→ext4 migration (ADR-0004).
- Until implemented, no behavior changes — this ADR records the direction and is
  Accepted only once a direction is chosen.
