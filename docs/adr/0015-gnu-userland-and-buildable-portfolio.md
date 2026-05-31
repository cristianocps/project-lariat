# ADR-0015: Robust GNU userland and a self-hosted, buildable app portfolio

- Status: Accepted
- Date: 2026-05-31

## Context

Lariat boots a clean Unix-like system with a Linux-x86_64-compatible syscall
ABI, musl as the system libc, dynamic linking, `lpkg`, and a cross + native
GCC/binutils toolchain (ADR-0001, 0002, 0003, 0012). The userland today is a
hand-written, busybox-style set of small tools (`userspace/*.c`) plus the `lcc`
bootstrap compiler. MicroPython runs as a dynamic PIE (ROADMAP N7).

The goal now is to become a **robust, general-purpose OS**: ship the real **GNU
userland** (coreutils, bash, grep, sed, gawk, make, tar/gzip, …) and be able to
**build a large portfolio of real applications**. The blocker is not "write more
tools" — it is the set of platform capabilities that real GNU software and
autotools `./configure` runs assume but Lariat does not yet fully provide:
terminals and job control, complete signals, a broader syscall surface, a mature
dynamic loader, and on-device build infrastructure.

Three strategic forks were decided (with the user):

1. **C library:** support **both**, sequenced — **musl first** (small,
   permissive, Linux-ABI; Alpine proves a huge portfolio builds on musl), with
   **glibc as a later track** for the apps that refuse to build against musl.
2. **Build model:** **self-hosted on-device builds first** — the proof and the
   primary path is `gcc` + `make` running *on Lariat* building the userland;
   host cross-compilation is the bootstrap that delivers the native toolchain,
   not the end state.
3. **First milestone:** a **self-sufficient GNU userland** — coreutils + bash +
   grep/sed/gawk + make + tar/gzip — that can rebuild itself on device.

## Decision

Adopt a **musl-first, self-hosting GNU userland** strategy, delivered in phases
(see ROADMAP Phases 6–10):

- **POSIX runtime first (Phase 6).** Land the capabilities real software needs:
  full signals (`sigaction`/masks/restart, `sigaltstack`, default actions,
  `SIGCHLD`), **PTYs + termios + job control** (`/dev/ptmx`, `/dev/pts/N`,
  sessions/process-groups, `tcsetpgrp`, `SIGWINCH/INT/TSTP`), a wider syscall
  surface (`epoll`/`ppoll`/`pselect`, `eventfd`, `mremap`, `statx`,
  `fcntl` locks, `utimensat`, real `getrandom` CSPRNG), and a mature dynamic
  loader (`dlopen`/`dlsym`, multi-`.so` graphs, `RUNPATH`/`ldconfig`).

- **Self-hosted toolchain + build infra (Phase 7).** Close the
  gcc-builds-gcc-on-device loop (finishing ADR-0012), and make GNU `make`,
  `pkg-config`, and autotools `configure` runs work on device.

- **GNU userland (Phase 8, the milestone).** Build on device and package as
  `lpkg`: coreutils, bash, grep, sed, gawk, findutils, tar, gzip, diffutils,
  make. Install to `/usr`; bash becomes the interactive/login shell. The lean
  in-tree tools stay in `/bin` as the always-present bootstrap fallback
  (ADR-0014: real files, PATH resolution — `/usr/bin` wins over `/bin`).

- **Ports/recipe system (Phase 9).** A declarative recipe format ("lports":
  fetch → verify → patch → configure → build → stage → `lpkg`) with a dependency
  graph and an on-device build driver (host-cross as the bootstrap). This is
  what scales from "one app" to a catalog (git, less, vim, full Python, …).

- **glibc track (Phase 10, later).** Add glibc as an alternate libc with
  multi-loader coexistence, only as needed for apps that won't build on musl.

## Consequences

- musl-first keeps the system small and the ABI Linux-compatible; the portfolio
  targets the (large) body of musl-buildable software, mirroring Alpine. glibc
  is deferred, not abandoned.
- Self-hosting-first raises the near-term bar (the native toolchain and build
  infra must actually run on device) but yields a system that can grow itself —
  the core of "robust, general-purpose."
- PTYs + job control + signals are a substantial kernel/driver effort and a hard
  prerequisite for bash and interactive tools; they are sequenced first so the
  rest of the userland has somewhere to run.
- The in-tree busybox-style tools become a bootstrap layer, not the product;
  they remain valuable for rescue and first-boot before `/usr` is populated.
- Package count, build times, and disk footprint grow; this pulls in dependent
  work already on the roadmap: `lpkg sync` at boot (N4), ext4 multi-level
  extents for large writes, larger/demand-paged `execve`, `/tmp`, and a minimal
  `C.UTF-8` locale.
