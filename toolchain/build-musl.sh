#!/usr/bin/env bash
#
# build-musl.sh - build musl libc for x86_64-lariat (static + shared).
#
# Why musl "just works": Lariat's syscall ABI deliberately matches Linux x86_64
# (same numbers, same calling convention, negative-errno returns - see
# include/uapi/syscall.h and docs/adr/0002). musl's stock x86_64 backend issues
# exactly those syscalls, so we build the upstream source against our sysroot and
# only override the dynamic loader name to ld-lariat.so.1.
#
# The subset of syscalls musl needs at runtime is implemented in Phase 0
# (cpu/syscall.c): mmap/mprotect/munmap, brk, *at family, rt_sigprocmask,
# readv/writev, futex, set_tid_address, clock_gettime, getrandom, etc.
#
# Prereq: ./make-sysroot.sh and the stage-1 cross gcc (build-binutils-gcc.sh).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/versions.sh"
export PATH="$TC_PREFIX/bin:$PATH"

mkdir -p "$TC_SRC"
file="musl-$MUSL_VER.tar.gz"
[ -f "$TC_SRC/$file" ] || (cd "$TC_SRC" && curl -fLO "https://musl.libc.org/releases/$file")
tar -C "$TC_SRC" -xzf "$TC_SRC/$file"
src="$TC_SRC/musl-$MUSL_VER"

# HEADERS_ONLY=1: install just the libc headers into the sysroot. This needs NO
# compiler (it only copies/derives headers) and MUST run before the stage-1
# gcc/libgcc build, which #includes them. Driven by build-all.sh.
if [ "${HEADERS_ONLY:-0}" = "1" ]; then
    echo "==> musl $MUSL_VER headers -> $SYSROOT"
    make -C "$src" ARCH=x86_64 prefix=/usr DESTDIR="$SYSROOT" install-headers
    echo "musl headers installed."
    exit 0
fi

# Name the Lariat dynamic loader. musl installs ld-musl-x86_64.so.1 by default;
# the kernel honors whatever PT_INTERP a binary carries (kernel/elf.c).
rm -rf "$TC_BUILD/musl"; mkdir -p "$TC_BUILD/musl"
( cd "$TC_BUILD/musl"
  "$src/configure" \
      --target="$LARIAT_TARGET" \
      --prefix=/usr \
      CC="$LARIAT_TARGET-gcc" \
      CROSS_COMPILE="$LARIAT_TARGET-"
  make
  make DESTDIR="$SYSROOT" install )

# musl installs /lib/ld-musl-x86_64.so.1 -> /usr/lib/libc.so itself; that is the
# PT_INTERP the cross gcc bakes into dynamic binaries. Keep a legacy ld-lariat.so.1
# alias too, for any binary still referencing the old name.
mkdir -p "$SYSROOT/lib"
ln -sf "../usr/lib/libc.so" "$SYSROOT/lib/ld-lariat.so.1" 2>/dev/null || true

echo "musl installed into $SYSROOT. Next: FULL=1 ./build-binutils-gcc.sh for C++."
