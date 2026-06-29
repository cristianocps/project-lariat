# ADR-0020: Evolution roadmap for 2026 — syscalls, filesystem, package manager, and desktop

- Status: Accepted
- Date: 2026-06-29

## Context

Lariat has reached a solid foundation: a preemptive SMP x86_64 kernel, a Linux-x86_64-compatible syscall ABI, musl dynamic linking, a writable ext4 data volume with firmlinks, a native on-device GCC toolchain, and an experimental desktop compositor. The next strategic question is how to prioritize the remaining work to become a general-purpose OS capable of running real applications and offering a coherent desktop experience.

Five trade-offs were surfaced and decided with the project owner:

1. Applications first or desktop first?
2. Is package security (checksums + signatures) mandatory from day one?
3. Does advanced filesystem work (ext4 symlinks/hard links + journal) come before the desktop?
4. Stay musl-first or start a glibc track now?
5. Full desktop environment or simple demo apps?

## Decision

### 1. Applications come first

The immediate priority is closing the POSIX runtime gaps that prevent real third-party software from running. This means completing the syscall surface, PTYs/termios/job control, and the GNU userland *before* polishing the graphical desktop. A working bash/vim/make/coreutils stack is the hard prerequisite for every other user-facing milestone.

### 2. Package security is mandatory from the start

`lpkg` 2.0 must ship with SHA-256 checksums and GPG-style signature verification on every package and repository index. Security is not a later polish step; an installable OS that fetches binaries over the network must never trust unauthenticated payloads.

### 3. Filesystem advances before desktop

ext4 symlinks, hard links, and a metadata journal are sequenced before the desktop work. They unblock the GNU userland (symlinks in `/usr`, hard links in builds) and provide crash consistency for the persistent data volume that the desktop and its apps will rely on.

### 4. musl-first remains the strategy

Lariat stays committed to musl as the system libc. A glibc track is acknowledged as future work (Phase 10) but is not started now. The musl-first path keeps the system small, the ABI Linux-compatible, and the portfolio aligned with Alpine-style buildability.

### 5. A complete desktop environment, seeded with simple apps

The desktop direction is macOS-inspired: a single cohesive environment with a Dock, Launcher, Finder-style file manager, global menu bar, notifications, and a polished window server. The first apps will be simple — Terminal, Calculator, Notes, TextEdit, and an expanded Settings — but they will be real, installable `lpkg` packages, not throwaway demos.

## Execution order

The agreed delivery order is:

1. **POSIX runtime completion** (Phase 6): `epoll`, `ppoll`/`pselect`, `eventfd`, `mremap`, `statx`, `utimensat`, `fcntl` locks, `clock_nanosleep`, `sysinfo`, real `getrandom`, full signals, PTYs/termios/job control.
2. **ext4 advanced features**: symlinks, hard links, metadata journal.
3. **GNU userland on device** (Phase 8): coreutils, bash, grep, sed, gawk, findutils, tar, gzip, diffutils, make as `lpkg` packages.
4. **`lpkg` 2.0**: remote index, versioned dependency resolution, SHA-256 + signatures, upgrade, cache, hooks.
5. **Terminal and PTYs**: `/dev/ptmx`, `/dev/pts/N`, a Terminal.app-style client.
6. **Desktop environment** (Phase 11): shared-memory window server, Dock, Launcher, Finder, simple apps.

## Consequences

- The GNU userland becomes the near-term milestone that unlocks the ports ecosystem.
- Package security is a first-class constraint for every repository and recipe.
- ext4 journal work is pulled forward from the backlog into the critical path.
- glibc is explicitly deferred; multi-loader coexistence is a future decision.
- The desktop is designed coherently from the start, even though the first apps are small.
