#!/usr/bin/env bash
#
# build-native-toolchain.sh - build a *native* binutils + GCC/G++ that RUN ON
# Lariat (Phase 5, self-hosting).
#
# This is a "Canadian cross": we use the already-built x86_64-lariat *cross*
# toolchain (from build-binutils-gcc.sh) as the build compiler, and configure
# binutils/gcc with:
#
#     --build=<host triple>      (the machine doing the compiling)
#     --host=x86_64-lariat       (the machine the tools will RUN on  -> Lariat)
#     --target=x86_64-lariat     (the machine the tools EMIT code for -> Lariat)
#
# host == target, so the produced as/ld/gcc/g++/cc1/cc1plus are Lariat ELF
# binaries that compile Lariat ELF binaries: an on-device toolchain.
#
# Prereqs:
#   1. ./make-sysroot.sh
#   2. ./build-binutils-gcc.sh           (cross binutils + stage-1 gcc)
#   3. ./build-musl.sh                   (musl into sysroot)
#   4. FULL=1 ./build-binutils-gcc.sh    (full cross gcc: libgcc + libstdc++)
#   5. this script                       (native binutils + gcc/g++)
#
# Output: a staged install tree at $NATIVE_PREFIX (default toolchain/native),
# laid out under /usr so it drops straight onto the Lariat root. Package it with
# ./package-native.sh, then `lpkg install` the result on-device.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/versions.sh"

# Where the native (Lariat-runnable) toolchain is staged. Gitignored.
export NATIVE_PREFIX="${NATIVE_PREFIX:-$TC_ROOT/native}"
# Prefix as seen *on Lariat* once installed (so configure bakes correct paths).
export TARGET_PREFIX="${TARGET_PREFIX:-/usr}"
export BUILD_TRIPLE="${BUILD_TRIPLE:-$(gcc -dumpmachine)}"

# The cross toolchain must already exist - it is our build compiler.
if [ ! -x "$TC_PREFIX/bin/$LARIAT_TARGET-gcc" ]; then
    echo "error: cross gcc not found at $TC_PREFIX/bin/$LARIAT_TARGET-gcc" >&2
    echo "       run the cross build first (see README 'Build order')." >&2
    exit 1
fi
export PATH="$TC_PREFIX/bin:$PATH"

# Use the cross tools as the compilers for the --host (Lariat) side.
export CC_FOR_HOST="$LARIAT_TARGET-gcc"
export CXX_FOR_HOST="$LARIAT_TARGET-g++"
export AR_FOR_HOST="$LARIAT_TARGET-ar"
export RANLIB_FOR_HOST="$LARIAT_TARGET-ranlib"

mkdir -p "$NATIVE_PREFIX" "$TC_BUILD/native-binutils" "$TC_BUILD/native-gcc"

echo "==> native binutils $BINUTILS_VER (runs on Lariat)"
rm -rf "$TC_BUILD/native-binutils"; mkdir -p "$TC_BUILD/native-binutils"
( cd "$TC_BUILD/native-binutils"
  "$TC_SRC/binutils-$BINUTILS_VER/configure" \
      --build="$BUILD_TRIPLE" --host="$LARIAT_TARGET" --target="$LARIAT_TARGET" \
      --prefix="$TARGET_PREFIX" --with-sysroot=/ \
      --disable-nls --disable-werror --disable-gdb \
      --disable-gprofng \
      CC="$CC_FOR_HOST" CXX="$CXX_FOR_HOST" AR="$AR_FOR_HOST" RANLIB="$RANLIB_FOR_HOST"
  make MAKEINFO=true
  make MAKEINFO=true DESTDIR="$NATIVE_PREFIX" install )

echo "==> native gcc/g++ $GCC_VER (runs on Lariat)"
rm -rf "$TC_BUILD/native-gcc"; mkdir -p "$TC_BUILD/native-gcc"
( cd "$TC_BUILD/native-gcc"
  "$TC_SRC/gcc-$GCC_VER/configure" \
      --build="$BUILD_TRIPLE" --host="$LARIAT_TARGET" --target="$LARIAT_TARGET" \
      --prefix="$TARGET_PREFIX" --with-sysroot=/ --with-build-sysroot="$SYSROOT" \
      --disable-nls --enable-languages=c,c++ --enable-default-pie \
      --enable-host-pie \
      --disable-bootstrap --disable-multilib \
      CC="$CC_FOR_HOST" CXX="$CXX_FOR_HOST" AR="$AR_FOR_HOST" RANLIB="$RANLIB_FOR_HOST"
  # all-host gives us the driver + cc1/cc1plus that run on Lariat. The target
  # libgcc/libstdc++ already live in the sysroot from the cross full build.
  make MAKEINFO=true all-host
  make MAKEINFO=true DESTDIR="$NATIVE_PREFIX" install-host )

echo "native toolchain staged at: $NATIVE_PREFIX$TARGET_PREFIX/bin"
echo "next: ./package-native.sh   (build lpkg packages)"
