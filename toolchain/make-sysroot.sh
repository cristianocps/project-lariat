#!/usr/bin/env bash
#
# make-sysroot.sh - assemble the x86_64-lariat sysroot.
#
# The sysroot is what the cross compiler reads headers from and links libraries
# against. We seed it with Lariat's public ABI headers (include/uapi) under
# /usr/include and create the standard library/binary directories. The C library
# (musl) headers and libs are added later by build-musl.sh.
#
# Output (sysroot/, gitignored):
#   usr/include/lariat/   <- public ABI: syscall.h, uapi.h
#   usr/include/          <- libc headers (filled by build-musl.sh)
#   usr/lib/  lib/  bin/   usr/bin/
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/versions.sh"

echo "assembling sysroot at $SYSROOT"
mkdir -p "$SYSROOT/usr/include/lariat" \
         "$SYSROOT/usr/lib" "$SYSROOT/lib" \
         "$SYSROOT/usr/bin" "$SYSROOT/bin"

# Public ABI headers (the kernel/userspace contract).
cp -v "$REPO_ROOT/include/uapi/syscall.h" "$SYSROOT/usr/include/lariat/"
cp -v "$REPO_ROOT/include/uapi/uapi.h"    "$SYSROOT/usr/include/lariat/"

# A tiny umbrella header so ported code can pull the Lariat ABI explicitly.
cat > "$SYSROOT/usr/include/lariat/lariat.h" <<'EOF'
#ifndef _LARIAT_H
#define _LARIAT_H
/* Umbrella for Lariat's public ABI. The syscall numbers match Linux x86_64,
 * which is why a stock musl x86_64 build runs with only a sysroot + dynamic
 * loader path change. */
#include <lariat/syscall.h>
#include <lariat/uapi.h>
#endif
EOF

echo "sysroot seeded. Next: ./build-binutils-gcc.sh then ./build-musl.sh"
