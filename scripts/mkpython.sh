#!/usr/bin/env bash
#
# mkpython.sh - cross-compile MicroPython against musl and package it as an
# .lpkg for Project Lariat (roadmap N7, "Python on Lariat" proof of concept).
#
# MicroPython's Unix "minimal" variant is a single small dynamic PIE that links
# only against musl libc - no external deps and no on-disk standard library - so
# it exercises Lariat's ELF loader, ld-musl, and a broad syscall surface without
# the packaging problem CPython's multi-thousand-file stdlib poses.
#
# The package ships:
#     bin/micropython              - the interpreter (musl PIE)
#     lib/ld-musl-x86_64.so.1      - the musl loader/libc (interpreter target)
#
# Install + run inside Lariat:
#     lpkg install /var/micropython-1.0.0.lpkg
#     micropython -c "print('hello')"
#
# Usage: scripts/mkpython.sh            (builds dist/packages/micropython-1.0.0.lpkg)
#   MPY_REF=v1.24.0 scripts/mkpython.sh (pin a MicroPython tag)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

. toolchain/versions.sh 2>/dev/null || true
TC_BIN="$ROOT/toolchain/install/bin"
CROSS="$TC_BIN/x86_64-linux-musl-gcc"
LIBC="$ROOT/sysroot/usr/lib/libc.so"
SRC="${MPY_SRC:-$ROOT/.local_libs/micropython}"
REF="${MPY_REF:-master}"
OUT="dist/packages"
VER="1.0.0"

[ -x "$CROSS" ] || { echo "error: musl cross gcc not found ($CROSS); run toolchain/build-all.sh" >&2; exit 1; }
[ -f "$LIBC" ]  || { echo "error: musl libc not found ($LIBC); build the toolchain first" >&2; exit 1; }

# Fetch MicroPython into a gitignored scratch dir.
if [ ! -d "$SRC/.git" ]; then
    mkdir -p "$(dirname "$SRC")"
    echo "==> cloning MicroPython ($REF) into $SRC"
    git clone --depth 1 --branch "$REF" https://github.com/micropython/micropython.git "$SRC" 2>/dev/null \
        || git clone --depth 1 https://github.com/micropython/micropython.git "$SRC"
fi

echo "==> building mpy-cross (host)"
make -C "$SRC/mpy-cross" -j"$(nproc)" >/dev/null

echo "==> building unix/minimal with $CROSS"
make -C "$SRC/ports/unix" VARIANT=minimal CC="$CROSS" LD="$CROSS" \
     STRIP=true SIZE=true -j"$(nproc)" >/dev/null

BIN="$SRC/ports/unix/build-minimal/micropython"
[ -f "$BIN" ] || { echo "error: micropython binary not produced" >&2; exit 1; }

STRIPPED="$(mktemp)"
cp "$BIN" "$STRIPPED"
strip "$STRIPPED" 2>/dev/null || true

mkdir -p "$OUT"
scripts/mklpkg.sh --name micropython --version "$VER" --arch x86_64 \
    --desc "MicroPython (musl, minimal) - Python on Lariat" \
    --out "$OUT/micropython-$VER.lpkg" \
    "0755:$STRIPPED:bin/micropython" \
    "0755:$LIBC:lib/ld-musl-x86_64.so.1"
rm -f "$STRIPPED"

echo "==> packaged: $OUT/micropython-$VER.lpkg"
ls -la "$OUT/micropython-$VER.lpkg"
