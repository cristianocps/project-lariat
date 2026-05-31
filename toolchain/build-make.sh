#!/usr/bin/env bash
#
# build-make.sh - cross-build GNU make for x86_64-lariat and package it as an
# installable LPKG1 (make-<ver>.lpkg).
#
# GNU make is the first build-system component of Phase 7b: it is the engine a
# `gcc`-rebuilds-`gcc` bootstrap (and any autotools `configure`) drives.  It is a
# self-contained C program that only needs musl libc, so it cross-builds with the
# Phase 1 cross compiler (toolchain/install/bin/x86_64-linux-musl-gcc) directly.
#
# The binary is a dynamic PIE (the toolchain default) and depends on the
# `libc-dev` package for the musl loader + libc.so at runtime -- NOT a static
# ET_EXEC, which would load at a low address that collides with kernel memory.
#
# Usage:  ./build-make.sh           (fetch + configure + build + package)
# Output: toolchain/packages/make-<ver>.lpkg
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/versions.sh"

OUT_DIR="${OUT_DIR:-$TC_ROOT/packages}"
MKLPKG="$REPO_ROOT/scripts/mklpkg.sh"
BUILD_HOST="$(cc -dumpmachine 2>/dev/null || gcc -dumpmachine)"
CROSS="$TC_PREFIX/bin/$LARIAT_TARGET-gcc"
SRC="$TC_SRC/make-$MAKE_VER"
BLD="$TC_BUILD/make"
DEST="$BLD/dest"      # DESTDIR staging for `make install`

[ -x "$CROSS" ] || { echo "error: cross compiler $CROSS missing; build the toolchain first" >&2; exit 1; }
mkdir -p "$TC_SRC" "$OUT_DIR"
export PATH="$TC_PREFIX/bin:$PATH"

echo "==> fetching GNU make $MAKE_VER"
if [ ! -d "$SRC" ]; then
    file="make-$MAKE_VER.tar.gz"
    [ -f "$TC_SRC/$file" ] || (cd "$TC_SRC" && curl -fLO "https://ftp.gnu.org/gnu/make/$file")
    tar -C "$TC_SRC" -xzf "$TC_SRC/$file"
fi

echo "==> configuring make ($LARIAT_TARGET, build=$BUILD_HOST)"
rm -rf "$BLD"; mkdir -p "$BLD"
( cd "$BLD"
  "$SRC/configure" \
      --host="$LARIAT_TARGET" --build="$BUILD_HOST" \
      --prefix=/usr --without-guile --disable-nls \
      CC="$CROSS" )

echo "==> building"
( cd "$BLD" && make )

echo "==> staging install"
rm -rf "$DEST"; mkdir -p "$DEST"
( cd "$BLD" && make install DESTDIR="$DEST" )

MAKE_BIN="$DEST/usr/bin/make"
[ -f "$MAKE_BIN" ] || { echo "error: $MAKE_BIN not produced" >&2; exit 1; }
"$TC_PREFIX/bin/$LARIAT_TARGET-strip" "$MAKE_BIN" 2>/dev/null || true
file "$MAKE_BIN"

echo "==> packaging make-$MAKE_VER.lpkg"
"$MKLPKG" --name make --version "$MAKE_VER" --deps "libc-dev" \
    --desc "GNU make - native x86_64-lariat" \
    --out "$OUT_DIR/make-$MAKE_VER.lpkg" \
    "0755:$MAKE_BIN:usr/bin/make"

echo "done: $OUT_DIR/make-$MAKE_VER.lpkg"
