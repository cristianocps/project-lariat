#!/usr/bin/env bash
#
# package-native.sh - turn the staged native toolchain (from
# build-native-toolchain.sh) into LPKG1 packages installable on Lariat.
#
# Produces three packages under $OUT_DIR (default toolchain/packages):
#
#   binutils-<ver>.lpkg   as, ld, ar, ranlib, ... + the BFD/opcodes libs
#   gcc-<ver>.lpkg        the C compiler driver, cc1, libgcc, headers
#   gpp-<ver>.lpkg        the C++ driver, cc1plus, libstdc++   (deps: gcc)
#
# Install on-device with, e.g.:
#   lpkg install /pkgs/binutils-2.42.lpkg
#   lpkg install /pkgs/gcc-14.1.0.lpkg
#   lpkg install /pkgs/gpp-14.1.0.lpkg     # pulls gcc as a dependency
#
# Then on Lariat:
#   echo 'int main(){return 0;}' > /tmp/t.c && gcc -o /tmp/t /tmp/t.c && /tmp/t
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/versions.sh"

NATIVE_PREFIX="${NATIVE_PREFIX:-$TC_ROOT/native}"
TARGET_PREFIX="${TARGET_PREFIX:-/usr}"
OUT_DIR="${OUT_DIR:-$TC_ROOT/packages}"
STAGE="$NATIVE_PREFIX$TARGET_PREFIX"
MKLPKG="$REPO_ROOT/scripts/mklpkg.sh"

[ -d "$STAGE/bin" ] || { echo "error: $STAGE/bin missing; run build-native-toolchain.sh first" >&2; exit 1; }
mkdir -p "$OUT_DIR"

# Collect MODE:SRC:DEST specs for every regular file under a staged subtree,
# rebasing it onto the on-device install prefix (strip $NATIVE_PREFIX).
collect() { # <subdir-under-stage> [extra find predicates...] -> appends to FILES[]
    local sub="$1"; shift
    local f mode dest
    while IFS= read -r -d '' f; do
        mode=$(stat -c '%a' "$f")
        dest="usr/${f#"$STAGE"/}"
        FILES+=("0$mode:$f:$dest")
    done < <(find "$STAGE/$sub" -type f "$@" -print0 2>/dev/null)
}

# binutils: assembler/linker/archiver and their support libs.
VER="$BINUTILS_VER"
FILES=()
for b in as ld ld.bfd ar ranlib nm objdump objcopy strip readelf addr2line size strings c++filt gprof; do
    [ -f "$STAGE/bin/$LARIAT_TARGET-$b" ] && FILES+=("0755:$STAGE/bin/$LARIAT_TARGET-$b:usr/bin/$b")
    [ -f "$STAGE/bin/$b" ] && FILES+=("0755:$STAGE/bin/$b:usr/bin/$b")
done
collect "lib/bfd-plugins" 2>/dev/null || true
"$MKLPKG" --name binutils --version "$VER" --deps "libc-dev" \
    --desc "GNU binutils (as, ld, ar, ...) - native x86_64-lariat" \
    --out "$OUT_DIR/binutils-$VER.lpkg" "${FILES[@]}"

# gcc: C driver + cc1 + libgcc + GCC's private headers/libexec.
#
# libgcc (crt*.o + libgcc.a/libgcc_eh.a + libgcc_s.so*) is the target runtime the
# link step needs, but `make install-host` does not place it under the native
# tree, so stage it from the cross build dir into lib/gcc/<triple>/<ver>/ first.
LIBGCC_BUILD="$TC_BUILD/gcc/$LARIAT_TARGET/libgcc"
LIBGCC_DST="$STAGE/lib/gcc/$LARIAT_TARGET/$GCC_VER"
if [ -d "$LIBGCC_BUILD" ] && [ -d "$LIBGCC_DST" ]; then
    for o in crtbegin.o crtbeginS.o crtbeginT.o crtend.o crtendS.o \
             libgcc.a libgcc_eh.a libgcc_s.so libgcc_s.so.1; do
        [ -f "$LIBGCC_BUILD/$o" ] && [ ! -f "$LIBGCC_DST/$o" ] && \
            cp "$LIBGCC_BUILD/$o" "$LIBGCC_DST/$o"
    done
fi
VER="$GCC_VER"
FILES=()
[ -f "$STAGE/bin/$LARIAT_TARGET-gcc" ] && FILES+=("0755:$STAGE/bin/$LARIAT_TARGET-gcc:usr/bin/gcc")
[ -f "$STAGE/bin/gcc" ] && FILES+=("0755:$STAGE/bin/gcc:usr/bin/gcc")
[ -f "$STAGE/bin/$LARIAT_TARGET-cpp" ] && FILES+=("0755:$STAGE/bin/$LARIAT_TARGET-cpp:usr/bin/cpp")
# Skip the C++/LTO backends here: cc1plus belongs to the gpp package and lto1 is
# only needed for -flto.  Excluding them keeps the C compiler package lean
# (cc1 + collect2 + lto-wrapper) for the on-device install.
collect "libexec/gcc" -not -name cc1plus -not -name lto1
# Skip lib/gcc/.../plugin: those are GCC-plugin development headers (hundreds of
# files) not needed to compile C/C++ on device, and they blow past lpkg's
# per-package file count.
collect "lib/gcc" -not -path '*/plugin/*'
"$MKLPKG" --name gcc --version "$VER" --deps "binutils" \
    --desc "GCC C compiler - native x86_64-lariat" \
    --out "$OUT_DIR/gcc-$VER.lpkg" "${FILES[@]}"

# g++: C++ driver + cc1plus + libstdc++. cc1plus already comes via libexec/gcc
# (collected by gcc), so g++ only adds the driver and the C++ runtime.
VER="$GCC_VER"
FILES=()
[ -f "$STAGE/bin/$LARIAT_TARGET-g++" ] && FILES+=("0755:$STAGE/bin/$LARIAT_TARGET-g++:usr/bin/g++")
[ -f "$STAGE/bin/g++" ] && FILES+=("0755:$STAGE/bin/g++:usr/bin/g++")
[ -f "$STAGE/bin/$LARIAT_TARGET-c++" ] && FILES+=("0755:$STAGE/bin/$LARIAT_TARGET-c++:usr/bin/c++")
"$MKLPKG" --name gpp --version "$VER" --deps "gcc" \
    --desc "GCC C++ compiler (g++) - native x86_64-lariat" \
    --out "$OUT_DIR/gpp-$VER.lpkg" "${FILES[@]}"

echo "packages written to $OUT_DIR:"
ls -1 "$OUT_DIR"/*.lpkg
