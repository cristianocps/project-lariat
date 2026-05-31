# ADR-0001: Cross-compile real applications first, self-host GCC later

- Status: Accepted
- Date: 2026-05-30

## Context

A goal of the roadmap is to run real GCC/G++-compiled applications on Lariat.
There are two fundamentally different ambitions:

1. **Cross-compile** real apps on a host PC with a toolchain that targets Lariat
   (apps run on Lariat; the compiler does not).
2. **Self-host**: run `gcc`/`g++` on Lariat itself.

Self-hosting requires a real libc, a real allocator, dynamic linking, a large
syscall surface, a writable filesystem with space, and on-device binutils+gcc.
It is an order of magnitude more work than cross-compilation.

## Decision

Pursue cross-compilation first as the near/medium-term target: define an
`x86_64-lariat` cross toolchain and a ported libc so that real applications can
be built on a host and executed on Lariat. Treat self-hosting GCC as a
long-term goal (Phase 5), gated on the foundations (Phase 0), the toolchain
(Phase 1), and the package manager (Phase 2).

## Consequences

- Phase 1 builds a cross GCC/G++/binutils and a ported libc, not an on-device
  compiler.
- The public ABI must be stabilized early (see ADR header split / Phase A) to
  seed the toolchain sysroot.
- Phase 5 remains in the plan but explicitly last and dependent on everything
  else being stable.
