# ADR-0002: Add dynamic linking and port musl as the system libc

- Status: Accepted
- Date: 2026-05-30

## Context

The in-tree libc is minimal (bump-allocator malloc, no math, no ISO stdio,
global errno) and every program is statically linked at a fixed address
`0x40000000` with no PIE, no relocations, and no dynamic loader. Broad
compatibility with real software (and C++/libstdc++) needs a real libc and the
ability to share it.

## Decision

Adopt dynamic linking: support `PT_INTERP` and `RELA` relocations in the ELF
loader, ship a dynamic loader (`ld-lariat.so`), and provide shared
`libc.so`/`libm.so`/`libstdc++.so`. Port **musl** as the system libc because it
is small, permissively licensed, and supports both static and dynamic linking
cleanly against a Linux-like syscall ABI.

The in-tree `userspace/libc` is retained only for the kernel-embedded bootstrap
programs.

## Consequences

- The ELF loader (`kernel/elf.c`) must place a full SysV auxv on the stack and
  process relocations (Phase 0 + Phase 1).
- A new mmap/file-mapping and `mprotect`/`munmap` surface is required (Phase 0).
- Programs become PIE/dynamic by default; the fixed `0x40000000` convention
  becomes a fallback for the bootstrap libc only.
- The package manager (Phase 2) must track shared-library dependencies.
