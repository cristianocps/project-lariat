# Project Lariat — Implementation & Deployment Roadmap

This is the living plan for Project Lariat: what has been built, what is in
progress, and what comes next. It complements `ARCHITECTURE.md` (how the system
is structured) and `adr/` (why each major decision was made). Each phase links
to the ADR(s) that record its rationale.

> **Status legend:** ✅ done · 🚧 in progress · 🔜 planned (next) · 🧊 backlog

## Vision

A 64-bit, from-scratch OS that can **build and run a large portfolio of real
applications** — shipping a genuine **GNU userland** (coreutils, bash, the build
tools) that the system can **rebuild on itself** — with an extensible desktop and
package ecosystem, evolving toward a hybrid (macOS/XNU-style) kernel with
first-class IPC and services, on a clean Unix-like filesystem hierarchy.

### Strategy decisions (see `adr/0015`)

- **C library — both, sequenced:** **musl first** (small, permissive,
  Linux-ABI; the Alpine-style path to a broad portfolio), **glibc as a later
  track** only for apps that refuse to build on musl.
- **Build model — self-hosting first:** the primary path is `gcc` + `make`
  running *on Lariat*; host cross-compilation is the bootstrap that delivers the
  native toolchain, not the destination.
- **First portfolio milestone:** a **self-sufficient GNU userland** (coreutils +
  bash + grep/sed/gawk + make + tar/gzip) that can rebuild itself on device.

## Phases

### Phase 0 — Core OS foundation ✅
x86_64 boot (real → protected → long mode), higher-half VMM, PMM, preemptive
multi-core SMP scheduler, SYSCALL/SYSRET fast path with Linux x86_64 numbering,
`fork`/`execve`/`wait4`, ELF64 loader, in-kernel TCP/IP stack, PS/2 + serial +
framebuffer + rtl8139 drivers.

### Phase 1 — Real-application toolchain ✅
- Cross-compile real apps first, self-host later — `adr/0001`.
- Dynamic linking + musl as the system libc (`ld.so`, PIE, `PT_INTERP`) — `adr/0002`.
- SSE/SSE2 enabled (BSP + APs) and FS/GS base preserved across syscalls, which
  unblocks musl/GCC-emitted binaries (TLS via `arch_prctl`).
- **Validated:** dynamic musl C/C++ binaries execute on-device (rc=42 test),
  loaded from the package volume.

### Phase 2 — Packaging & extensibility ✅
- `LPKG1` package format and the `lpkg` package manager — `adr/0003`.
- Self-hosting native toolchain packaged for on-device builds — `adr/0012`.

### Phase 3 — Desktop & settings ✅
- Display server protocol for the desktop — `adr/0005`.
- GUI Settings app — `adr/0011`.

### Phase 4 — System configuration & accounts ✅
- procfs, `/etc` configuration, and init services — `adr/0009`.
- Account administration + stronger password hash — `adr/0010`.

### Phase M — Hybrid kernel direction ✅ (foundations) / 🧊 (deepening)
- Hybrid (macOS/XNU-style) kernel direction — `adr/0006`.
- IPC ports and the service manager — `adr/0007`.
- Backlog: migrate more in-kernel services behind IPC ports.

### Phase 5 — Storage & persistence ✅
- Persistent writable root filesystem — `adr/0004`.
- Block I/O foundation: split >256-sector ATA transfers into hardware-safe
  chunks; write-through sector cache that coalesces cold-miss runs.
- Reliable IDE secondary-channel detection (`-device ide-hd,bus=ide.1`,
  `cache=writethrough`, longer ATA soft-reset/settle).
- **Writable ext4** in-kernel: extents, block/inode bitmap alloc+free, dir entry
  insert/remove (idempotent `create`/`mkdir`), inode writeback, truncate,
  deleted-inode stamping. `e2fsck -fn` clean across create/write/mkdir/unlink/
  rmdir cycles and across reboots.
- **Persistence migrated off FAT32 onto ext4**: system state (`/etc`), the
  package database (`/var/lib/lpkg`), and package delivery now live on the ext4
  volume. Validated by a two-boot seed→restore cycle, fsck-clean.

## Near-term plan (next)

### N1 — Unified filesystem namespace ✅
Replaced the device/type-named mount points (`/disk` = FAT32, `/ext4` = ext4 —
effectively "drive letters") with a single Unix-like hierarchy: the ext4 data
volume mounts at `/var`, FAT32 at `/mnt/legacy`, and `/etc` + `/home` are
macOS-style **firmlinks** (symlinks) into `/var`. Required adding symbolic-link
support to the VFS (path-resolution following, ramfs symlink inodes, syscalls)
and retiring the boot-time copy-in + `etc_sync` mirroring. Backing device/type
is now metadata, not part of the path. See `adr/0013` (Accepted, Option B).

### N2 — Formal filesystem documentation ✅
`FILESYSTEM.md`: the namespace, mount sequence, VFS contract, symlink/firmlink
model, persistence story, ext4 on-disk subset, and the recovery/fsck story.

### N3 — FAT32 as a removable-media format (not the default) 🔜
ext4 (`/var`) stays the default, authoritative on-disk filesystem. FAT32 is
**kept** — not as the system store, but as the interchange format for mounting
external/removable units in the future (e.g. under `/mnt/<label>` or
`/Volumes/<label>`). Demote the hardcoded `/mnt/legacy` mount to an on-demand
mount and keep the driver read-write.

### N6 — Mount table + fstab parser, rescue path ✅
Only the essentials (ramfs `/`, ext4 `/var`, procfs `/proc`) are bootstrapped in
`kmain`; everything else is data-driven. A kernel **mount table** records each
mount and backs **`/proc/mounts`**. After the `/etc` firmlink is up, **`mount_fstab()`**
parses `/etc/fstab` and brings up the remaining volumes (e.g. FAT32 at
`/mnt/legacy`), skipping already-mounted and `noauto` entries. If `/var` fails
to mount, the boot enters a **ramfs-only rescue mode** (factory defaults, no
persistence) instead of failing. Verified: `/proc/mounts` lists all volumes,
fstab brings up FAT32, and a zeroed `/var` device boots to login in rescue mode.

### N4 — Package persistence completeness 🧊
Run `lpkg sync` at boot so installed package payloads are re-extracted from the
persistent DB into the (volatile) system tree after a reboot.

### N7 — Existing-application compatibility: Python 🚧
Prove and grow the ability to run real third-party interpreters. Staged:
1. **MicroPython** (musl, minimal variant) ✅ — a single ~190 KB dynamic PIE,
   few syscalls, no stdlib-on-disk. Cross-compiled via `scripts/mkpython.sh`,
   packaged as an `.lpkg`, installed and run at boot (`init` self-test prints
   real Python output, rc=0). First "Python runs on Lariat" proof.
2. **CPython minimal** (`python -c`) — cross-compiled with `--enable-shared`,
   stdlib *frozen* or shipped as a single `python3xx.zip` (zipimport) to sidestep
   the `lpkg` 16 MiB/256-file limit and the 16 MiB execve cap on static builds.
3. **CPython + stdlib** — broaden syscall coverage as gaps surface.

Known syscall gaps to close along the way: `mremap`, `epoll_*`, `ppoll`,
`eventfd`, `sigaltstack`, `set_robust_list`, `sysinfo`, true `lstat`
(`AT_SYMLINK_NOFOLLOW`), and a stronger `getrandom` CSPRNG.

### N5 — ext4 read performance 🧊
Coalesce contiguous extent blocks into larger `block_read`s. (Low priority — the
write-through block cache already makes large reads adequate.)

### N8 — PATH-based executable resolution + real /bin ✅
Command resolution is now plain `$PATH` file lookup, the way Linux/macOS do it,
not a hardcoded in-kernel table. ext4 now persists inode metadata
(`mode`/`uid`/`gid` via a VFS `setattr` op), so a user can own `~/.local/bin` on
`/var`; the embedded programs are **materialized as real files in `/bin`** at
boot (the in-kernel table is only a fallback); and `login(8)` composes `$PATH`
per account (system `*bin` for root, `~/.local/bin` + `~/bin` for users).
Verified: a non-root user installs and runs a command from `~/.local/bin`, it
survives reboot, and the image stays `e2fsck`-clean. See `adr/0014`.

### N9 — kernel.c decluttering ✅
Moved boot-world setup, the SMP self-tests, and the in-kernel debug shell out of
`kernel.c` into `kernel/core/` (`world.c`, `smptest.c`, `kshell.c`), leaving
`kernel.c` as essentially just `kmain`. The debug shell is now
registration-based (`kshell_register`) instead of a hardcoded dispatch chain.

## Future direction — robust GNU userland & a buildable portfolio (`adr/0015`)

This is the strategic arc toward a general-purpose OS. The ordering is
deliberate: the runtime capabilities real software assumes come first, then the
on-device toolchain, then the GNU userland, then a ports system to scale the
catalog. musl-first throughout; glibc is a later track.

### Phase 6 — POSIX runtime for real software 🔜
The capabilities GNU software and autotools `./configure` runs assume. This
phase is the hard prerequisite for bash and every interactive tool.
- **6a — Signals completeness:** full `sigaction`/masks/restart, `sigaltstack`,
  default actions, correct `SIGCHLD`/stop/continue semantics.
- **6b — PTYs + termios + job control:** `/dev/ptmx` + `/dev/pts/N`, termios
  ioctls, sessions/process-groups (`setsid`, `setpgid`, `tcsetpgrp`/`tcgetpgrp`),
  controlling terminal, `SIGWINCH`/`SIGINT`/`SIGTSTP`. *The single biggest
  unlock — bash, vim, less, make's job server all need it.*
- **6c — Syscall surface:** `epoll_*`/`ppoll`/`pselect`, `eventfd`, `mremap`,
  `statx`, `fcntl` locks, `utimensat`, `clock_nanosleep`, `sysinfo`, true
  `lstat`, and a real `getrandom` CSPRNG (subsumes the N7 syscall-gap list).
- **6d — Dynamic loader maturity:** `dlopen`/`dlsym`, multi-`.so` dependency
  graphs, `RUNPATH`/search, an `ldconfig`-equivalent cache.
- **6e — Runtime plumbing:** `/tmp`, env + minimal `C.UTF-8` locale,
  larger/demand-paged `execve`, ext4 multi-level extents for large/fragmented
  writes, and `lpkg sync` at boot (folds in N4).

### Phase 7 — Self-hosted toolchain & build infrastructure 🚧
Make the system able to build software *on itself*.
- **7a — gcc compiles & runs a program on device** ✅. The native
  `binutils`/`gcc` (+ `libc-dev` sysroot) packages install on `/var` via `lpkg`,
  and `gcc` compiles a C source to a working executable that runs on Lariat —
  with the **LTO linker plugin enabled** (gcc's default, no special flags):

  ```
  gcc /var/test.c -o /tmp/prog && /tmp/prog
  HELLO_FROM_LARIAT_GCC rc=42
  ```

  Closing this exposed and fixed six foundational gaps (see `adr/0016`):
  1. **Linux-ABI `struct stat`** — the syscall ABI is now byte-for-byte Linux
     x86_64 (`st_nlink` before `st_mode`, full 144-byte layout). The musl
     toolchain was reading `st_mode` from the wrong offset, so `cc1` rejected
     every header directory as "not a directory."
  2. **`execve` unshares a `CLONE_VM` address space** — `posix_spawn`
     (`CLONE_VM|CLONE_VFORK`) is how `gcc` launches `cc1`/`as`/`ld`. exec from a
     shared address space now allocates a fresh page table instead of resetting
     in place, which had been wiping the parent driver's mappings (null-call
     crash in the `gcc` driver).
  3. **`umask` implemented** — `ld` (BFD) `chmod`s its output executable using
     `umask`; the missing syscall yielded a non-executable mode, so freshly
     linked binaries failed `execve` with `EACCES`.
  4. **`libgcc` packaged** — `crt{begin,end}*.o`, `libgcc.a`, `libgcc_eh.a`,
     `libgcc_s.so*` are now in the gcc `.lpkg`; the link step needs them.
  5. **6th syscall argument** — the syscall entry stub never passed argument 6
     to the C handler (it read the thread pointer instead), so `mmap`'s `offset`
     was garbage. Any runtime `dlopen` then mapped a file of zeros and crashed;
     this is why the **LTO plugin** appeared to need `dlopen` support — `dlopen`
     already worked, the file offset was just wrong.
  6. **`execve` argv limit** — argv was capped at 31 entries. Enabling the LTO
     plugin makes `collect2` drive `ld` with ~45 arguments, so the *trailing*
     `crtendS.o`/`crtn.o` were silently dropped → `hidden symbol __TMC_END__
     isn't defined`. The limit is now 512 args / 128 KiB.

  *Known limitations (tracked for later phases):* **`-static`** produces a
  low-address `ET_EXEC` that collides with kernel memory (dynamic PIE is the
  working path); and `g++` (`libstdc++`) is deferred.
- **7b — rebuild gcc itself on device** 🚧 (the full `adr/0012` loop). The
  compiler now builds and links arbitrary C programs on device, and **GNU `make`
  drives multi-step `gcc` builds** (see below); the remaining work is the
  autotools `configure` chain, then the full `gcc`-rebuilds-`gcc` bootstrap.
- **GNU `make` is up** ✅ (`adr/0017`). Cross-built and packaged as
  `make-<ver>.lpkg` (dynamic PIE, deps `libc-dev`); a bare `make` builds a
  multi-file project on device (`gcc -c` per source, link, `make clean`). This
  required fixing a latent kernel gap: the VFS only accepted absolute paths, so
  **relative paths are now resolved against the process cwd** at the syscall
  boundary (`open`/`stat`/`unlink`/`rename`/`execve`/…) — the norm for builds
  (`cc -c main.c`). The in-tree `rm` also learned `-f` (ubiquitous in recipes).
- **`dash` is the `configure` shell** ✅ (`adr/0018`). The hand-made `/bin/sh`
  lacks the constructs autotools `configure` requires, so we cross-built **dash**
  (Debian's POSIX `/bin/sh`) and packaged `dash-<ver>.lpkg`, installed as both
  `/usr/bin/dash` and `/usr/bin/sh` (PATH puts `/usr/bin` first → `sh` is dash;
  the in-tree `/bin/sh` stays as the bootstrap fallback). Proven on device:
  functions, `&&`/`||`, `${x%.c}`, `$(… | …)`, `case`, `for`, `[ ]`, `$((…))`,
  here-docs, and dash as `make`'s `SHELL`. Bringing dash up exposed and fixed
  **four kernel gaps**: (1) fds are now released at process **exit**, not at
  reap — dash's `$(...)` reads a pipe to EOF *before* `wait`, so a zombie holding
  the write end deadlocked the reader; (2) **`vfork`** (syscall 58) was unwired,
  so dash's vfork-to-exec of simple commands got `ENOSYS` ("Cannot fork") — it is
  now aliased to a full `fork`; (3) `clear_child_tid` is reset across `execve`
  and (4) its exit-time write is bounds-checked, so a stale futex pointer can no
  longer page-fault the kernel. The in-tree `wc` also learned `-l`/`-w`/`-c`.
- **GNU `m4` is up** ✅. Cross-built and packaged as `m4-<ver>.lpkg` (dynamic
  PIE, deps `libc-dev`, `toolchain/build-m4.sh`); autoconf is built on m4, and
  `configure` scripts are generated by it. Proven on device end-to-end:
  `define`/expansion, `ifelse`, `eval` (`2**10` → 1024), `len`, `translit`,
  nested macros. No kernel changes were needed — the dash-era fork/exec/pipe
  fixes already cover it.
- Still to bring up: **`pkg-config`** and the rest of the **autotools
  `configure`** path (coreutils + `sed`/`grep`/`awk`), plus `diffutils` as
  `configure` needs them — the shell and m4 are no longer the blockers.

### Phase 8 — GNU userland (the milestone) 🔜
Build on device and package as `lpkg`, installing to `/usr`: **coreutils, bash,
grep, sed, gawk, findutils, tar, gzip, diffutils, make.** bash becomes the
interactive/login shell. The lean in-tree tools stay in `/bin` as the
always-present bootstrap fallback (`adr/0014`: `/usr/bin` wins over `/bin` on
PATH). **Success:** a from-scratch GNU userland that rebuilds itself on device.

### Phase 9 — Ports system & portfolio growth 🧊
**lports** — a declarative recipe format (fetch → verify → patch → configure →
build → stage → `lpkg`) with a dependency graph and an on-device build driver
(host-cross as the bootstrap). Scales from "one app" to a catalog. Folds in the
existing Python track (N7): full CPython + stdlib becomes a port. Early targets:
`git`, `less`, `vim`/`nano`, `python3`.

### Phase 10 — glibc track 🧊
Add glibc as an alternate libc with multi-loader coexistence, only as needed for
apps that won't build against musl. Lower priority by design.

## Conventions

- Land an ADR in the same change that implements the decision (`adr/README.md`).
- Keep this roadmap updated as phases land; it is the single source of truth for
  "where we are and what's next."
