#!/usr/bin/env bash
#
# package-sysroot.sh - package the target musl sysroot as an lpkg `libc-dev`
# package so the on-device toolchain can find the C/C++ headers, link against
# crt*.o / libc.so, and (critically) so the dynamic loader the toolchain
# binaries themselves need is present on the target.
#
# Produces $OUT_DIR/libc-dev-<musl>.lpkg containing:
#   /usr/include/...                 C/C++ headers (musl)
#   /usr/lib/{crt*.o,libc.so,...}    link-time objects and libraries
#   /usr/lib/ld-musl-x86_64.so.1     musl dynamic loader (== libc.so)
#
# The loader lives under the persistent /usr prefix (firmlinked to /var/usr);
# the kernel firmlinks /lib/ld-musl-x86_64.so.1 -> /var/usr/lib/... at boot so
# the PT_INTERP path still resolves.
#
# Install it first on-device:
#   lpkg install /pkgs/libc-dev-1.2.5.lpkg
#   lpkg install /pkgs/binutils-2.42.lpkg
#   lpkg install /pkgs/gcc-14.1.0.lpkg
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/versions.sh"

OUT_DIR="${OUT_DIR:-$TC_ROOT/packages}"
MKLPKG="$REPO_ROOT/scripts/mklpkg.sh"
SR="$SYSROOT"

[ -d "$SR/usr/include" ] || { echo "error: $SR/usr/include missing; run make-sysroot.sh + build-musl.sh" >&2; exit 1; }
[ -f "$SR/usr/lib/libc.so" ] || { echo "error: $SR/usr/lib/libc.so missing; run build-musl.sh" >&2; exit 1; }
mkdir -p "$OUT_DIR"

FILES=()

# C/C++ headers -> /usr/include (preserve subdir layout: bits/, sys/, arpa/...).
while IFS= read -r -d '' f; do
    mode=$(stat -c '%a' "$f")
    FILES+=("0$mode:$f:usr/include/${f#"$SR"/usr/include/}")
done < <(find "$SR/usr/include" -type f -print0)

# Link-time objects + libraries -> /usr/lib (crt1.o, Scrt1.o, crti/crtn,
# libc.so, libc.a, libm.a, ...).
while IFS= read -r -d '' f; do
    mode=$(stat -c '%a' "$f")
    FILES+=("0$mode:$f:usr/lib/${f#"$SR"/usr/lib/}")
done < <(find "$SR/usr/lib" -type f -print0)

# The dynamic loader the toolchain (and any dynamic binary) needs at runtime.
# In musl the loader and libc are the same image; ship it under the PT_INTERP
# basename but inside the persistent /usr/lib prefix (the kernel firmlinks
# /lib/ld-musl-x86_64.so.1 -> /var/usr/lib/ld-musl-x86_64.so.1 at boot).
if [ -f "$SR/lib/ld-musl-x86_64.so.1" ]; then
    FILES+=("0755:$SR/lib/ld-musl-x86_64.so.1:usr/lib/ld-musl-x86_64.so.1")
else
    FILES+=("0755:$SR/usr/lib/libc.so:usr/lib/ld-musl-x86_64.so.1")
fi

"$MKLPKG" --name libc-dev --version "$MUSL_VER" --deps "" \
    --desc "musl libc headers, link libraries and dynamic loader - x86_64-lariat" \
    --out "$OUT_DIR/libc-dev-$MUSL_VER.lpkg" "${FILES[@]}"

echo "wrote $OUT_DIR/libc-dev-$MUSL_VER.lpkg (${#FILES[@]} files)"
