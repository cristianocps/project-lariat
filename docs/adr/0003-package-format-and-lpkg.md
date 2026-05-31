# ADR-0003: Package format and the `lpkg` package manager

- Status: Accepted
- Date: 2026-05-30

## Context

Today, `/bin` programs are embedded into the kernel image at build time, and the
`execve` path checks an embedded registry first. There is no way to install or
extend the system without a kernel rebuild, no dependency tracking, and no
versioning. We want installable apps (a desktop environment, calculator, text
editor) and, eventually, on-device toolchain packages.

## Decision

Define a simple package format and a userspace tool `lpkg`
(`userspace/lpkg.c`).

- **Format (`LPKG1`)**: a plain-text header followed by the concatenated raw
  contents of each file — no compression and no external archiver dependency,
  so a package is created with `printf`/`cat` (`scripts/mklpkg.sh`) and parsed
  in a single linear pass with the in-tree libc:

  ```
  LPKG1
  name=<name>
  version=<ver>
  arch=<arch>
  deps=<comma-separated, may be empty>
  desc=<one line>
  %FILES
  <octal-mode> <decimal-size> <relative/dest/path>   (one line per file)
  %DATA
  <raw bytes of each file, in %FILES order>
  ```

  (We chose this over tar/MANIFEST because Lariat has no in-OS tar yet; the
  format can be swapped for tar later without changing the DB or tool surface.)
- **Database**: on the persistent FAT32 root (ADR-0004) at
  `/disk/var/lib/lpkg/db/<name>/` with `meta` (metadata), `files` (installed
  absolute paths for clean removal), and `archive` (a cached copy of the
  `.lpkg`).
- **Install layout**: packaged files are extracted to their absolute paths in
  the live tree (`/usr/{bin,lib,share}`) with the recorded mode. Because the
  root tree is volatile ramfs while the DB is persistent, `lpkg sync`
  re-extracts every recorded package from its cached archive on boot.
  (Executables must land in ramfs because FAT32 cannot represent the execute
  bit.)
- **Commands**: `install` / `remove` / `list` / `info` / `sync`, with
  dependency checking on install (each declared dep must already be recorded;
  overridable with `--force`).
- **Sources**: local package files first; a network repository fetched over
  HTTP using the existing TCP stack lands a `.lpkg` in a temp path and reuses
  the same local install path.

## Consequences

- `kernel/elf.c` now resolves on-disk binaries *first* and falls back to the
  embedded blobs only when the path is absent on disk, so packages add or
  override tools without a kernel rebuild. The shell searches `PATH`
  (`/bin`, `/usr/bin`, `/usr/local/bin`).
- Requires a writable, persistent root (Phase 0) for the package database.
- Dependency resolution is initially simple (depends list, no version ranges);
  it can grow later. True root-on-disk persistence (mounting `/usr`/`/var`)
  remains future work, bridged for now by `lpkg sync`.
