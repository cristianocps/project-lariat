# ADR-0014: PATH-based executable resolution and a real /bin

- Status: Accepted
- Date: 2026-05-31

## Context

Early Lariat resolved commands through a hardcoded in-kernel table: core
utilities lived as blobs in `kernel/elf.c` (`g_embedded`), and `execve` matched
the requested path against that table before touching the filesystem. This was
expedient for bring-up but is not how Unix/macOS work, and it had real costs:

- `/bin` was not a real directory. `ls`, `cp`, and `stat` could not see the
  utilities, so they could not be copied, inspected, or overridden — the kernel
  was the source of truth for the command set.
- There was no clean, extensible way to add OS-wide commands (you rebuilt the
  kernel) or **user-specific** commands (impossible — there was no per-user
  install location).
- The ext4 driver reconstructed a VFS inode's mode from the coarse directory
  entry type and never read `uid`/`gid`, so it could not persist `chmod`/`chown`
  or creator ownership. A user could not own a directory on `/var`, which blocks
  any per-user install location.

We want command resolution to be plain filesystem lookup over `$PATH`, with both
an OS-wide location and a per-user one, the way Linux and macOS do it.

## Decision

Adopt **PATH-based executable resolution** backed by real files:

1. **ext4 inode metadata persistence.** The ext4 driver loads the real on-disk
   `i_mode` (type + permission bits) and `i_uid`/`i_gid` on lookup, and a new
   VFS `setattr` inode op (`vfs_setattr` → `ext4_setattr`) writes them back on
   `chmod`/`chown` and on creator ownership for `mkdir`/`open(O_CREAT)`. ramfs
   needs no write-back (its in-core inode is the source of truth).

2. **Materialize `/bin` as real files.** At boot, `world_setup()` writes every
   embedded `/bin/*` program out as a real `0755` file
   (`elf_install_embedded_programs`). `g_embedded` is retained only as a
   last-resort exec fallback. `/bin` is now an ordinary, inspectable, extensible
   directory.

3. **`$PATH` search in userspace.** `sh` splits `$PATH` on `:` and execs the
   first match (a name with `/` is a literal path). `login(8)` composes `$PATH`
   per account: root gets the `*sbin` + `*bin` system trees; an ordinary user
   gets `~/.local/bin` and `~/bin` prepended to the system `*bin` trees.
   `/etc/profile` seeds the system-wide default.

## Consequences

- A user can `mkdir ~/.local/bin`, `cp` a binary in, and run it by bare name —
  on the persistent `/var` volume, surviving reboot and `e2fsck`-clean. Verified
  end-to-end.
- `/bin` behaves like a normal Unix directory; coreutils can be copied, stated,
  and overridden by packages (`lpkg` installs already win, since the on-disk
  file is resolved before the embedded fallback).
- The kernel sheds responsibility for the command namespace: adding commands is
  a filesystem/package concern, not a kernel rebuild. This complements the
  `kernel/core` decluttering (boot world, SMP self-tests, and the debug shell
  moved out of `kernel.c`).
- Remaining limits: ext4 writes use depth-0 extents only; the embedded table
  still exists as a safety net (it could be trimmed to just `/init` + `/bin/sh`
  once package-driven `/bin` is the norm). See `docs/FILESYSTEM.md` §7.
