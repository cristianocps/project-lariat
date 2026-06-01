# ADR-0019: Persistent `/usr` prefix via firmlinks and ext4 extent-tree writes

- Status: Accepted
- Date: 2026-06-01

## Context

Lariat's system tree (`/`, `/bin`, `/usr`, `/lib`) was volatile ramfs, rebuilt
from the kernel image every boot. Persistence was confined to the ext4 data
volume mounted at `/var`, exposed to the namespace as macOS-style **firmlinks**:
`/etc -> /var/etc` and `/home -> /var/home` (ADR-0013). Package installs,
however, write under `/usr` — which was volatile — so a reboot lost them. The
stopgap (roadmap N4) was `lpkg sync`: cache each package archive in the
persistent DB and **re-extract** every package into the volatile tree at boot.

That model is wrong for the toolchain workload of Phase 7. Re-extracting tens of
megabytes (gcc's `cc1`/`lto1`) on every boot is slow and fragile, and it
defeats the "immutable system, persistent data" split we already committed to
for `/etc` and `/home`. The natural fix is to finish that model: make the
**install prefix itself persistent** with a `/usr -> /var/usr` firmlink, so
`lpkg install` writes to disk once and packages simply *stay* installed.

Doing so was blocked by an ext4 **large-file write bug**. `ext4_file_write`
relied on `ext4_extent_append`, which only supported the 4 inline (depth-0)
extents stored in the inode; once full it returned `-1` and the writer `break`ed
**silently**, truncating the file. A 43 MB `cc1` written to `/var/usr` came out
truncated and unrunnable. So a persistent `/usr` is impossible without real
extent-tree writes.

## Decision

### 1. `/usr` (and the musl loader) become firmlinks onto `/var`

`world_setup` (`kernel/core/world.c`), alongside the existing `/etc` and `/home`
firmlinks, now creates `/var/usr` and links `/usr -> /var/usr`. The one
`PT_INTERP` path that lives outside `/usr` — the musl dynamic loader — is
handled by repackaging `libc-dev` to install the loader at
`/usr/lib/ld-musl-x86_64.so.1` (i.e. `/var/usr/lib/...`) and firmlinking
`/lib/ld-musl-x86_64.so.1` onto it. ELF binaries keep their stock
`PT_INTERP = /lib/ld-musl-x86_64.so.1`, resolved through the firmlink.

Both links are created only when the `/var` volume mounted (guarded by
`g_fs_rescue`). In **rescue mode** there is no `/usr` or loader firmlink, so
dynamically-linked installed apps cannot run — but `/bin` is statically-linked
and materialized from the kernel image every boot, so the system always reaches
a usable login. `/bin` stays immutable and is the PATH/rescue fallback (`/usr/bin`
still wins over `/bin` on PATH, ADR-0014).

### 2. ext4 gains full extent-tree writes (`kernel/fs/ext4.c`)

The read path already descended a multi-level tree; the write path now matches
it:

- **Tree growth.** When the inline depth-0 list fills, `ext4_grow_depth`
  allocates a leaf block, moves the inline extents into it, and converts the
  inode's `i_block` into a depth-1 **index** header. The tree grows further as
  needed (`ext4_extent_map` splits leaf/index nodes and propagates upward).
- **Contiguous allocation.** `ext4_alloc_block_run` hands out runs of
  contiguous blocks so a large file uses few extents (less fragmentation, fewer
  tree nodes).
- **Sorted insertion.** Writes are no longer assumed append-only. A linker
  (`ld`) back-seeks and patches its output, producing **out-of-order logical
  blocks**. `ext4_leaf_put`/`ext4_idx_put` insert each extent in sorted
  logical-block order (coalescing adjacent ones), because `e2fsck` requires
  extents within a node to be logically ascending. `ext4_file_write` dispatches
  sequential appends to the fast `ext4_extent_append` path and out-of-order
  writes to `ext4_extent_map`.
- **Tree-aware free.** `ext4_free_blocks_from`/`ext4_free_subtree_from` recurse
  the tree on truncate/unlink, freeing data **and** index/leaf metadata blocks,
  and canonicalize an emptied inode back to depth-0.

Result: gcc's `cc1` and linker output write correctly and `e2fsck -fn` stays
clean across large/fragmented files, truncate, and unlink.

### 3. VFS `..` resolution across the ext4 mount (`kernel/vfs.c`)

Exercising the real toolchain on `/var/usr` exposed a path-resolution bug. gcc
locates its backend by a `..`-relative path
(`/usr/bin/../libexec/gcc/.../cc1`). Disk filesystems mint a fresh dentry per
`lookup` and cannot know the parent dentry, so `..` ascended to the wrong node
and the lookup failed (`gcc` fell back to running `cc1` by bare name →
`posix_spawnp: No such file or directory`). `vfs_walk` now anchors each resolved
child to the directory it was looked up in (`next->parent = current`), so a
later `..` ascends to the real parent across the ext4 volume.

### 4. `lpkg` retires the archive cache and `sync` (`userspace/lpkg.c`)

`lpkg install` no longer copies the package archive into the DB
(`db/<name>/archive`) — that copy existed only to feed boot-time re-extraction
and was itself a victim of the truncation bug. The DB under `/var/lib/lpkg`
still records each install for `list`/`info`/`remove`. `cmd_sync` is now a
no-op that reports the installed-package count and explains that `sync` is
obsolete now that `/usr` persists.

## Consequences

- **Install once, survive reboot, fast boot.** The whole toolchain
  (`libc-dev`, `binutils`, `gcc`, `make`, `dash`, `m4`) installs once to
  `/var/usr`; after a reboot `gcc --version` and a full compile work with **no
  `lpkg sync`**, and boot is a few milliseconds regardless of install size
  (boot only mounts `/var` and re-creates the firmlinks). Verified end-to-end
  with `e2fsck -fn` clean.
- **The "immutable system + persistent data" model is now complete.** `/usr`
  joins `/etc` and `/home` as a firmlink; `/bin` and the kernel image remain
  immutable and reconstructed each boot.
- **General correctness wins.** The extent-tree write path fixes large-file
  writes for *any* workload, and the `vfs_walk` reparenting fixes `..` across
  every disk mount, not just gcc's lookup.
- **Deferred:** the ext4 journal (the volume stays crash-consistent via
  write-through, not journaling); `RUNPATH`-based multi-loader coexistence
  (glibc track, Phase 10).
