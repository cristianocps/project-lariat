# ADR-0012: Self-hosting - a native toolchain packaged for on-device builds

- Status: Accepted
- Date: 2026-05-30

## Context

Phase 1 produced an `x86_64-lariat` *cross* toolchain (binutils + GCC/G++ + musl;
ADR-0001, ADR-0002) that runs on a host and emits Lariat binaries. Phase 5 is the
self-hosting milestone: the compiler should run *on Lariat*, and be deliverable
through the package manager (`lpkg`, ADR-0003) rather than baked into the kernel
image.

Two things make this tractable without inventing anything new:

- Lariat's syscall ABI is Linux-x86_64-compatible, and the syscall surface a
  native binutils/GCC needs is already implemented and dispatched in
  `cpu/syscall.c` - notably `openat`/`newfstatat`/`faccessat`, `rename`,
  `unlink`/`mkdir`/`rmdir`, `ftruncate` (+ the ramfs `truncate` fix from
  ADR-0010), `getdents64`, `readlink`/`symlink`, `chmod`, `clock_gettime`,
  `uname`, the `*v`/`p*` I/O variants, `mmap`/`mprotect`, and full
  `fork`/`execve`/`wait4`.
- The bundled `lcc` (a Lariat C-subset compiler, `userspace/lcc.c`) already
  proves the end-to-end *compile-on-device-then-run* path, exercised headlessly
  by an `init` self-test.

## Decision

- **Native (Canadian-cross) toolchain build**
  (`toolchain/build-native-toolchain.sh`): reuse the Phase 1 cross compiler as
  the build compiler and configure binutils/GCC with
  `--build=<host> --host=x86_64-lariat --target=x86_64-lariat`. Because
  `host == target`, the produced `as`/`ld`/`gcc`/`g++`/`cc1`/`cc1plus` are Lariat
  ELF binaries that compile Lariat ELF binaries. The target `libgcc`/`libstdc++`
  already come from the cross *full* build in the sysroot, so the native build is
  `make all-host` / `install-host` into a staging prefix rooted at `/usr`.

- **Ship as `lpkg` packages** (`toolchain/package-native.sh`): split the staged
  tree into three LPKG1 packages with dependencies -
  `binutils` (as/ld/ar/...), `gcc` (C driver + `cc1` + `libgcc`, deps:
  `binutils`), and `gpp` (g++ driver + `libstdc++`, deps: `gcc`). Installing
  `gpp` pulls the chain via `lpkg`'s dependency resolution.

- **`lcc` is the always-present bootstrap compiler**: it stays embedded in the
  kernel image so a bare disk can compile-and-run before any package is
  installed, and it backs the headless self-test. The full GCC packages are the
  production path once installed.

- **No `/bin` baked toolchain**: per ADR (Phase 2 decoupling), the native
  compiler resolves via `PATH` from `/usr/bin` after `lpkg install`, not from the
  kernel-embedded program table.

## Consequences

- The full native build needs network access and a lengthy host build; the
  scripts are pinned (`versions.sh`) and idempotent but are intentionally run
  out-of-band, not in the boot/CI fast path. Their staging/output dirs
  (`toolchain/native/`, `toolchain/packages/`) are gitignored.
- Self-hosting is validated continuously at the *capability* level (syscall
  surface + `lcc` compile/run self-test); a full `gcc`-rebuilds-`gcc` loop is a
  follow-up that depends only on running the two scripts above and installing the
  packages.
- This also surfaced and fixed a latent limit: the kernel image had grown past
  the bootloader's 512 KB load window. The window was raised to 896 KB
  (`boot/boot.asm`: `KERNEL_CHUNKS=56`) with headroom, since embedding more
  userspace tools (settings, account tools, `lcc`) pushed `kernel.bin` over the
  edge and truncated tail blobs.
