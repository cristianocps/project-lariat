# x86_64-lariat cross toolchain

Builds a GCC/G++/binutils cross compiler targeting Lariat plus a ported musl
libc, so real C/C++ applications can be cross-compiled on a host and run on
Lariat. See `docs/adr/0001-cross-compile-then-self-host.md` and
`docs/adr/0002-dynamic-linking-and-musl.md`.

## Key idea

Lariat's syscall ABI is intentionally Linux-x86_64-compatible (same syscall
numbers and calling convention, negative-errno returns; see
`include/uapi/syscall.h`). That means:

- binutils/gcc treat `x86_64-lariat` as a generic ELF SysV target.
- **musl's stock x86_64 backend works** with only a sysroot and a dynamic-loader
  name change - no per-syscall porting layer is required, because Phase 0
  already implemented the syscalls musl issues.

## Build order

```sh
cd toolchain
./make-sysroot.sh                 # seed sysroot/usr/include/lariat from uapi
./build-binutils-gcc.sh           # binutils + stage-1 C cross gcc
./build-musl.sh                   # musl libc into the sysroot
FULL=1 ./build-binutils-gcc.sh    # full gcc: C/C++, libgcc, libstdc++, PIE
```

Result: `toolchain/install/bin/x86_64-lariat-gcc` (and `-g++`), producing
dynamic, PIE ELF executables whose `PT_INTERP` is `/lib/ld-lariat.so.1` (loaded
by the kernel ELF loader, `kernel/elf.c`).

## Defaults

- `--enable-default-pie`: executables are PIE/`ET_DYN`; the kernel loads them at
  `USER_DYN_BASE` and applies `R_X86_64_RELATIVE` relocations.
- Dynamic loader: `/lib/ld-lariat.so.1` (musl's loader, installed under that
  name in the sysroot and on the target root).

## Self-hosting: a native toolchain that runs on Lariat (Phase 5)

Once the cross toolchain and sysroot exist, build a **native** binutils + GCC/G++
(a Canadian cross with `--host=x86_64-lariat`) so the compiler itself runs on
Lariat, then ship it as `lpkg` packages. See
`docs/adr/0012-self-hosting-native-toolchain.md`.

```sh
cd toolchain
./build-native-toolchain.sh       # native as/ld/gcc/g++ staged in toolchain/native
./package-native.sh               # binutils-*.lpkg, gcc-*.lpkg, gpp-*.lpkg
```

On the Lariat device (or in the image), install and use them:

```sh
lpkg install /pkgs/binutils-2.42.lpkg
lpkg install /pkgs/gcc-14.1.0.lpkg
lpkg install /pkgs/gpp-14.1.0.lpkg     # depends on gcc
echo 'int main(){return 0;}' > /tmp/t.c && gcc -o /tmp/t /tmp/t.c && /tmp/t
```

`lcc` (the bundled Lariat C-subset compiler, `userspace/lcc.c`) is the minimal,
always-present on-device compiler used to prove the compile-and-run path in the
`init` self-tests before the full GCC packages are installed.

## Outputs (all gitignored)

- `toolchain/install/` - the cross compiler
- `toolchain/native/` - the native (Lariat-runnable) toolchain staging tree
- `toolchain/packages/` - the produced `.lpkg` packages
- `toolchain/src/`, `toolchain/build/` - sources and out-of-tree build dirs
- `sysroot/` - target headers + libraries

## Smoke test (after a full build)

```sh
export PATH=$PWD/install/bin:$PATH
echo 'int main(){return 0;}' > /tmp/t.c
x86_64-lariat-gcc -o /tmp/t /tmp/t.c
file /tmp/t      # ELF 64-bit PIE, dynamically linked, interpreter /lib/ld-lariat.so.1
```

Copy the resulting binary plus the sysroot's `/lib/ld-lariat.so.1` and
`/usr/lib/libc.so` onto the Lariat disk image (or install via `lpkg`, Phase 2)
and run it.
