# ADR-0017: On-device builds — GNU make and cwd-relative path resolution

- Status: Accepted
- Date: 2026-05-31

## Context

ADR-0016 closed the C compile-and-run loop: `gcc` builds and links a working
program on device, with the LTO linker plugin enabled. The next step toward a
`gcc`-rebuilds-`gcc` bootstrap (Phase 7b) is a **build system** — GNU `make`,
the engine every `Makefile` and autotools `configure` drives.

`make` is a self-contained C program that needs only musl libc, so it
cross-builds with the Phase 1 cross compiler without ceremony. But running it on
device immediately exposed a latent kernel gap: **the VFS only accepts absolute
paths.** `vfs_lookup_path()` rejects any path not starting with `/`, and the
path syscalls (`open`, `stat`, `access`, `unlink`, `mkdir`, `rename`, …) passed
user paths straight through. Only `chdir` tracked a current working directory.

Programs that hard-code absolute paths (the shell, `gcc` invoked with absolute
arguments) worked, which is why this stayed hidden. But `make` — and the
recipes it runs — are built around *relative* paths: it `stat`s `Makefile`,
`main.c`, `main.o` in the current directory, and runs `gcc -c main.c -o main.o`.
Every one of those resolved against nothing and failed, so `make` reported
`No targets specified and no makefile found` and `No rule to make target
'main.c'` even though the files were right there.

## Decision

1. **Resolve relative paths against the process cwd at the syscall boundary.**
   A small `cwd_join()` helper in `cpu/syscall.c` prepends the calling thread's
   `cwd` to any path that does not begin with `/`, producing one absolute path
   before any VFS call. `vfs_walk()` already collapses `.`/`..` components, so a
   plain join is sufficient. It is applied to every path-taking syscall:
   `open`/`openat`, `stat`/`newfstatat`, `access`/`faccessat`, `chmod`,
   `readlink`, `symlink` (link location only), `rename` (both operands),
   `mkdir`, `rmdir`, `unlink`, and `execve`. Resolving at the boundary (rather
   than inside the VFS) keeps the VFS free of thread/scheduler coupling and means
   the `O_CREAT` path (`vfs_lookup_parent`, which also requires absolute) is
   covered for free.

2. **Cross-build and package GNU make as `make.lpkg`** (`toolchain/build-make.sh`,
   pinned `MAKE_VER` in `versions.sh`). It is configured `--host=x86_64-linux-musl
   --build=<host> --without-guile` with the cross `gcc`, producing a **dynamic
   PIE** (not a static `ET_EXEC`, which would load at a low address that collides
   with kernel memory). The package depends on `libc-dev` for the musl loader and
   `libc.so`. This is the first non-toolchain *application* package; it follows
   the same `mklpkg.sh` LPKG1 conventions as binutils/gcc.

3. **Make the in-tree `rm` accept `-f`** (`userspace/rm.c`). Recipes lean on
   `rm -f` constantly (`make clean`). `rm` now parses any combination of
   `-r`/`-R`/`-f`, and `-f` ignores nonexistent operands and never fails — the
   bootstrap `rm` should not choke on the most common form in Makefiles.

## Consequences

- **GNU make drives multi-step `gcc` builds on device.** Proven end-to-end with
  a two-source project built by a bare `make` (makefile auto-discovery, relative
  source paths, `rm -f` clean):

  ```
  lpkg install /var/pkgs/make-4.4.1.lpkg
  cd /var/proj && make
  gcc -c main.c -o main.o
  gcc -c util.c -o util.o
  gcc main.o util.o -o prog
  ./prog  →  MAKE_BUILT_OK sum=42
  ```

- **Relative paths now work everywhere**, which unblocks not just `make` but any
  imported program that uses cwd-relative I/O (the common case). This is a
  correctness fix that should have predated the toolchain work.

- **Remaining for 7b:** the autotools `configure` chain (a capable `/bin/sh`,
  the coreutils/`sed`/`grep`/`awk` set, `m4`) and then the full
  `gcc`-rebuilds-`gcc` bootstrap. `chdir` still stores the cwd string without
  normalizing `..`; harmless for `vfs_walk` resolution but worth tidying when
  `getcwd`-sensitive tools (deeper autotools runs) arrive.
