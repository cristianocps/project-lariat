#!/usr/bin/env bash
#
# build-all.sh - build the complete x86_64-linux-musl cross toolchain for Lariat
# in the one order that satisfies the libgcc<->libc dependency cycle.
#
# Why this order (the subtle part):
#   - target libgcc #includes libc headers, so libc HEADERS must exist first;
#   - the shared musl libc links against libgcc soft-helpers (e.g. __mulsc3),
#     so libgcc must exist before the FULL musl build;
#   - libstdc++ (full gcc) needs the full musl libc.
#
# Steps:
#   1. make-sysroot.sh                       seed ABI headers + dirs
#   2. STAGE=binutils    build-binutils-gcc  assembler/linker
#   3. HEADERS_ONLY=1    build-musl.sh        libc headers only (no compiler)
#   4. STAGE=gcc-stage1  build-binutils-gcc  C compiler + target libgcc
#   5.                   build-musl.sh        full static+shared musl libc
#   6. STAGE=gcc-full    build-binutils-gcc  C/C++, libstdc++, PIE
#
# Result: toolchain/install/bin/x86_64-linux-musl-{gcc,g++,...}. Then build the
# native (on-device) toolchain with ./build-native-toolchain.sh.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

./make-sysroot.sh
STAGE=binutils    ./build-binutils-gcc.sh
HEADERS_ONLY=1    ./build-musl.sh
STAGE=gcc-stage1  ./build-binutils-gcc.sh
                  ./build-musl.sh
STAGE=gcc-full    ./build-binutils-gcc.sh

echo
echo "cross toolchain ready: $(pwd)/install/bin/x86_64-linux-musl-g++"
echo "next: ./build-native-toolchain.sh && ./package-native.sh"
