# ADR-0016: Closing the self-hosting loop (gcc compiles & runs on device)

- Status: Accepted
- Date: 2026-05-31

## Context

ADR-0012 delivered the native `binutils`/`gcc` packages and validated
self-hosting at the *capability* level (syscall surface + the `lcc` self-test).
Phase 7a is the first *real* proof: install the native `binutils`, `gcc`, and a
`libc-dev` sysroot package via `lpkg`, then have `gcc` compile a C source into a
working executable that runs on Lariat.

Driving the real GNU `gcc` end-to-end (rather than the in-tree `lcc`) exposed
four foundational gaps. The toolchain is `x86_64-linux-musl`: it produces
Linux-ABI binaries and its libc (musl) assumes the **Linux x86_64 syscall ABI**.
Lariat already used Linux syscall *numbers* and a Linux-compatible `dirent64`,
but had diverged on a few structures and behaviors that only a full compiler
exercises.

Symptoms observed, in the order they unblocked:

1. `cc1` printed `warning: /usr/include: not a directory` for every header
   directory and could not find `stdio.h` — even though `cd /usr/include`
   worked. A Lariat-native shell could enter the directory; the musl `cc1` could
   not *stat* it as one.
2. With headers found, the `gcc` *driver* then crashed with a null-call
   (`rip=0x0`) whenever it spawned a sub-process (`cc1`/`as`/`ld`) — even for
   `-E`, after the real output was already written.
3. With the driver fixed, the link stage crashed inside `ld` while loading the
   LTO plugin, and after side-stepping that, freshly linked binaries refused to
   `execve` ("command not found" / `EACCES`).

## Decision

Treat the syscall ABI as **byte-for-byte Linux x86_64** (the whole point of the
`x86_64-linux-musl` toolchain) and fix the specific divergences:

1. **`struct stat` = Linux x86_64 layout.** `struct kstat` (`include/uapi/uapi.h`)
   and the in-tree libc `struct stat` are now the exact 144-byte Linux layout:
   `st_dev, st_ino, st_nlink, st_mode, st_uid, st_gid, __pad0, st_rdev, st_size,
   st_blksize, st_blocks, st_atim…`. The previous "simplified" layout put
   `st_mode` at offset 16; musl reads it at offset 24, so every directory looked
   like a non-directory. In-tree userland was recompiled against the new layout.

2. **`execve` unshares a `CLONE_VM` address space instead of resetting it.**
   musl's `posix_spawn` uses `clone(CLONE_VM|CLONE_VFORK)` and is how `gcc`
   launches `cc1`/`as`/`ld`. When such a child `execve`s, the kernel now
   allocates a fresh page table, drops its reference to the shared mm, and
   becomes sole owner of the new space — leaving the spawning parent's mappings
   intact. The old code reset the *shared* address space in place, which wiped
   the still-running `gcc` driver (the `rip=0x0` null-call crash).

3. **`umask(2)` implemented.** A per-process file-creation mask (default `022`,
   inherited across fork/clone/exec) backs `sys_umask`. `ld` (BFD) makes its
   output executable via `chmod(name, 0777 & ~umask & (mode|0111))`; with
   `umask` returning `-ENOSYS`, BFD computed a non-executable mode and the binary
   failed `execve`. (The `struct stat` fix was a prerequisite — BFD first
   `stat`s the file.)

4. **`libgcc` is packaged with gcc.** `crt{begin,end}{,S,T}.o`, `libgcc.a`,
   `libgcc_eh.a`, and `libgcc_s.so*` (built under
   `toolchain/build/gcc/x86_64-linux-musl/libgcc/`) are installed into the
   native tree and collected into the gcc `.lpkg`; the link step requires them.

Secondary correctness fix found along the way: `clock_gettime` now maps the
per-process / per-thread CPU-time clocks (`CLOCK_PROCESS_CPUTIME_ID`,
`CLOCK_THREAD_CPUTIME_ID`) to the monotonic source, so GCC's `timevar` deltas are
non-decreasing (a garbage report previously tripped an internal `validate_phases`
assertion when `cc1` was run standalone).

## Consequences

- **The loop is closed for the C / dynamic-PIE path.** On device:

  ```
  lpkg install /var/pkgs/libc-dev-1.2.5.lpkg
  lpkg install /var/pkgs/binutils-2.42.lpkg
  lpkg install /var/pkgs/gcc-14.1.0.lpkg
  gcc -fno-use-linker-plugin hello.c -o hello && ./hello
  ```

  `gcc` finds its own `cc1`/`as`/`ld` (no `-B` needed once `stat` is correct) and
  the resulting dynamic PIE runs.

- **Aligning the ABI to Linux is now an explicit invariant.** Any new
  kernel/userland shared structure must match the Linux x86_64 layout, not a
  convenient simplification, so imported musl/Linux-ABI binaries keep working.

- **Known limitations (follow-ups):**
  - The **LTO linker plugin** is `dlopen`ed by `ld`; runtime `dlopen` is not yet
    supported (Phase 6d), so links pass `-fno-use-linker-plugin` for now.
  - **`-static`** yields a low-address `ET_EXEC` that collides with kernel
    memory; the dynamic PIE path is the working one until the loader grows a
    static-exec/static-PIE placement story.
  - **`g++`/`libstdc++`** is deferred; the C package excludes `cc1plus`/`lto1`
    and the plugin-dev headers to stay within `lpkg` size/file limits.
  - The **"`gcc` rebuilds `gcc`"** loop (7b) remains; it depends on `make` and a
    broader on-device build environment (Phase 7 continued).
