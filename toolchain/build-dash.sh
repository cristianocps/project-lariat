#!/usr/bin/env bash
#
# build-dash.sh - cross-build the dash shell for x86_64-lariat and package it as
# an installable LPKG1 (dash-<ver>.lpkg).
#
# dash is a small, strictly-POSIX /bin/sh: the shell Debian/Ubuntu use precisely
# because it runs autotools `configure` scripts correctly and fast.  Phase 7b's
# `configure` chain needs a real shell (the in-tree sh lacks &&, ||, $(...),
# case, functions, parameter expansion, here-docs, ...); dash provides them in a
# tiny dynamic-PIE binary.
#
# The package installs the binary as BOTH /usr/bin/dash and /usr/bin/sh (two
# copies; the on-device lpkg does not need symlink support).  Because PATH puts
# /usr/bin ahead of /bin (ADR-0014), `sh` then resolves to dash, while the
# in-tree /bin/sh stays as the bootstrap fallback.
#
# Usage:  ./build-dash.sh           (fetch + configure + build + package)
# Output: toolchain/packages/dash-<ver>.lpkg
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/versions.sh"

OUT_DIR="${OUT_DIR:-$TC_ROOT/packages}"
MKLPKG="$REPO_ROOT/scripts/mklpkg.sh"
BUILD_HOST="$(cc -dumpmachine 2>/dev/null || gcc -dumpmachine)"
CROSS="$TC_PREFIX/bin/$LARIAT_TARGET-gcc"
SRC="$TC_SRC/dash-$DASH_VER"
BLD="$TC_BUILD/dash"

[ -x "$CROSS" ] || { echo "error: cross compiler $CROSS missing; build the toolchain first" >&2; exit 1; }
mkdir -p "$TC_SRC" "$OUT_DIR"
export PATH="$TC_PREFIX/bin:$PATH"

echo "==> fetching dash $DASH_VER"
if [ ! -d "$SRC" ]; then
    file="dash-$DASH_VER.tar.gz"
    [ -f "$TC_SRC/$file" ] || (cd "$TC_SRC" && \
        curl -fLO "http://gondor.apana.org.au/~herbert/dash/files/$file")
    tar -C "$TC_SRC" -xzf "$TC_SRC/$file"
fi

echo "==> configuring dash ($LARIAT_TARGET, build=$BUILD_HOST)"
rm -rf "$BLD"; mkdir -p "$BLD"
# BUILD_CC must be the *host* compiler: dash compiles small generator tools
# (mksyntax/mknodes/...) that run during the build on the build machine.
( cd "$BLD"
  "$SRC/configure" \
      --host="$LARIAT_TARGET" --build="$BUILD_HOST" \
      --prefix=/usr --enable-static=no \
      CC="$CROSS" BUILD_CC="cc" )

echo "==> building"
( cd "$BLD" && make BUILD_CC="cc" )

DASH_BIN="$BLD/src/dash"
[ -f "$DASH_BIN" ] || { echo "error: $DASH_BIN not produced" >&2; exit 1; }
"$TC_PREFIX/bin/$LARIAT_TARGET-strip" "$DASH_BIN" 2>/dev/null || true
file "$DASH_BIN"

echo "==> packaging dash-$DASH_VER.lpkg"
"$MKLPKG" --name dash --version "$DASH_VER" --deps "libc-dev" \
    --desc "dash - POSIX shell (also installed as /usr/bin/sh) - x86_64-lariat" \
    --out "$OUT_DIR/dash-$DASH_VER.lpkg" \
    "0755:$DASH_BIN:usr/bin/dash" \
    "0755:$DASH_BIN:usr/bin/sh"

echo "done: $OUT_DIR/dash-$DASH_VER.lpkg"
