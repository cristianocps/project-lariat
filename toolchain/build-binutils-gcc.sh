#!/usr/bin/env bash
#
# build-binutils-gcc.sh - build the x86_64-linux-musl cross binutils + GCC/G++.
#
# Produces a cross toolchain in toolchain/install/bin (x86_64-linux-musl-gcc,
# -g++, -ld, ...) that targets Lariat. Lariat's ABI is Linux-x86_64-compatible
# (ELF64, System V psABI, musl libc, identical syscall numbers), so we use the
# stock `x86_64-linux-musl` triple and need NO config.sub/OS-config patching.
#
# This script runs in STAGEs because of a hard ordering constraint: target
# libgcc needs libc *headers*, and the shared musl libc needs libgcc. The order
# (see build-all.sh, which drives this) is:
#
#   1. binutils                         STAGE=binutils
#   2. musl headers (build-musl.sh HEADERS_ONLY=1)
#   3. gcc stage-1 + target libgcc      STAGE=gcc-stage1   (needs musl headers)
#   4. full musl (build-musl.sh)
#   5. full gcc: C/C++, libstdc++, PIE  STAGE=gcc-full     (needs full musl)
#
# Downloads pinned sources; needs network and a host C/C++ toolchain. Re-running
# reuses src/. MAKEINFO=true skips the texinfo manuals (no makeinfo dependency).
#
# Back-compat: with no STAGE set, FULL=0 runs binutils+gcc-stage1 and FULL=1 runs
# gcc-full, but those paths assume musl headers/libc already exist in the sysroot.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/versions.sh"

STAGE="${STAGE:-}"
FULL="${FULL:-0}"
mkdir -p "$TC_SRC" "$TC_BUILD" "$TC_PREFIX"
export PATH="$TC_PREFIX/bin:$PATH"

fetch() { # url
    local url="$1" file="${1##*/}"
    [ -f "$TC_SRC/$file" ] || (cd "$TC_SRC" && curl -fLO "$url")
    case "$file" in
        *.tar.xz) tar -C "$TC_SRC" -xf "$TC_SRC/$file" ;;
        *.tar.gz) tar -C "$TC_SRC" -xzf "$TC_SRC/$file" ;;
    esac
}

ensure_sources() {
    echo "==> fetching sources"
    fetch "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz"
    fetch "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz"
    if [ ! -d "$TC_SRC/gcc-$GCC_VER/gmp" ]; then
        echo "==> downloading GCC prerequisites (gmp/mpfr/mpc)"
        ( cd "$TC_SRC/gcc-$GCC_VER" && ./contrib/download_prerequisites )
    fi
}

do_binutils() {
    echo "==> binutils $BINUTILS_VER for $LARIAT_TARGET"
    rm -rf "$TC_BUILD/binutils"; mkdir -p "$TC_BUILD/binutils"
    ( cd "$TC_BUILD/binutils"
      "$TC_SRC/binutils-$BINUTILS_VER/configure" \
          --target="$LARIAT_TARGET" --prefix="$TC_PREFIX" \
          --with-sysroot="$SYSROOT" --disable-nls --disable-werror
      make MAKEINFO=true && make MAKEINFO=true install )
}

# Stage-1 C compiler PLUS target libgcc. Requires libc headers in the sysroot
# (build-musl.sh HEADERS_ONLY=1) so libgcc - which #includes <stdio.h> et al -
# compiles. libgcc provides the soft helpers (e.g. __mulsc3) the shared musl
# libc links against, breaking the libgcc<->libc cycle.
do_gcc_stage1() {
    echo "==> gcc $GCC_VER stage-1 (C) + target libgcc"
    rm -rf "$TC_BUILD/gcc"; mkdir -p "$TC_BUILD/gcc"
    ( cd "$TC_BUILD/gcc"
      "$TC_SRC/gcc-$GCC_VER/configure" \
          --target="$LARIAT_TARGET" --prefix="$TC_PREFIX" \
          --with-sysroot="$SYSROOT" --disable-nls --disable-multilib \
          --enable-languages=c --disable-shared --disable-threads --disable-libssp
      make MAKEINFO=true all-gcc
      make MAKEINFO=true all-target-libgcc
      make MAKEINFO=true install-gcc install-target-libgcc )
}

do_gcc_full() {
    echo "==> gcc $GCC_VER full (C/C++, libgcc, libstdc++) against sysroot"
    rm -rf "$TC_BUILD/gcc"; mkdir -p "$TC_BUILD/gcc"
    ( cd "$TC_BUILD/gcc"
      "$TC_SRC/gcc-$GCC_VER/configure" \
          --target="$LARIAT_TARGET" --prefix="$TC_PREFIX" \
          --with-sysroot="$SYSROOT" --disable-nls --disable-multilib \
          --disable-libsanitizer \
          --enable-languages=c,c++ --enable-shared --enable-default-pie
      make MAKEINFO=true && make MAKEINFO=true install )
    echo "full toolchain done: $TC_PREFIX/bin/$LARIAT_TARGET-g++"
}

ensure_sources
case "$STAGE" in
    binutils)   do_binutils ;;
    gcc-stage1) do_gcc_stage1 ;;
    gcc-full)   do_gcc_full ;;
    "")
        # Back-compat default: no explicit STAGE.
        if [ "$FULL" = "0" ]; then
            do_binutils
            do_gcc_stage1
            echo "stage-1 done. Run ./build-musl.sh, then FULL=1 ./build-binutils-gcc.sh"
        else
            do_gcc_full
        fi ;;
    *) echo "unknown STAGE=$STAGE" >&2; exit 2 ;;
esac
