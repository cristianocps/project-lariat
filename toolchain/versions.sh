#!/usr/bin/env bash
# Pinned component versions and shared paths for the x86_64-lariat toolchain.
# Sourced by the other toolchain scripts.

# Target triple. Lariat's ABI is deliberately identical to Linux x86_64 (same
# syscall numbers/convention, ELF64, System V psABI, musl libc), so we build with
# the stock `x86_64-linux-musl` triple. This means binutils/gcc need NO config.sub
# or OS-config patching: GCC already ships full linux+musl specs (dynamic linker,
# libstdc++, PIE). The produced binaries are Linux-ABI ELF, which is exactly what
# the Lariat kernel runs. The kernel honors whatever PT_INTERP the binary carries
# (see kernel/elf.c), so the musl loader name below is what we ship on the target.
# (A custom `x86_64-lariat` triple would require patching config.sub + adding a
# gcc OS config; rejected as needless churn given the Linux-compatible ABI.)
export LARIAT_TARGET="x86_64-linux-musl"

# Component versions (override by exporting before sourcing).
export BINUTILS_VER="${BINUTILS_VER:-2.42}"
export GCC_VER="${GCC_VER:-14.1.0}"
export MUSL_VER="${MUSL_VER:-1.2.5}"

# Where everything lives. Build artifacts; all gitignored.
export TC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export REPO_ROOT="$(cd "$TC_ROOT/.." && pwd)"
export TC_PREFIX="${TC_PREFIX:-$TC_ROOT/install}"      # cross compiler install
export SYSROOT="${SYSROOT:-$REPO_ROOT/sysroot}"        # target sysroot
export TC_SRC="${TC_SRC:-$TC_ROOT/src}"                # downloaded sources
export TC_BUILD="${TC_BUILD:-$TC_ROOT/build}"          # out-of-tree build dirs

# The dynamic loader path baked into produced dynamic executables (PT_INTERP).
# With the x86_64-linux-musl triple, GCC emits musl's standard loader path; the
# kernel ELF loader (kernel/elf.c) reads PT_INTERP and loads whatever it finds
# there, so we ship the loader under this name on the target root.
export LARIAT_DYNLINKER="/lib/ld-musl-x86_64.so.1"

export MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}"
