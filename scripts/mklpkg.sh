#!/usr/bin/env bash
#
# mklpkg.sh - build an LPKG1 package for Project Lariat's `lpkg` tool.
#
# The package format is a plain-text header followed by the concatenated raw
# contents of each file (see userspace/lpkg.c and
# docs/adr/0008-package-format-and-lpkg.md):
#
#     LPKG1
#     name=<name>
#     version=<ver>
#     arch=<arch>
#     deps=<comma-separated>
#     desc=<one line>
#     %FILES
#     <octal-mode> <decimal-size> <relative/dest/path>     (one per file)
#     %DATA
#     <raw bytes of each file, in %FILES order>
#
# Usage:
#   scripts/mklpkg.sh --name N --version V [--arch x86_64] [--deps "a,b"] \
#       [--desc "text"] --out OUT.lpkg  MODE:SRC:DEST [MODE:SRC:DEST ...]
#
# Example:
#   scripts/mklpkg.sh --name hello --version 1.0.0 --desc "demo" \
#       --out hello.lpkg  0755:userspace/hello.elf:usr/bin/hello
set -euo pipefail

NAME="" VER="" ARCH="x86_64" DEPS="" DESC="" OUT=""
FILES=()
while [ $# -gt 0 ]; do
    case "$1" in
        --name) shift; NAME="$1" ;;
        --version) shift; VER="$1" ;;
        --arch) shift; ARCH="$1" ;;
        --deps) shift; DEPS="$1" ;;
        --desc) shift; DESC="$1" ;;
        --out) shift; OUT="$1" ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) FILES+=("$1") ;;
    esac
    shift
done

[ -n "$NAME" ] || { echo "mklpkg: --name required" >&2; exit 2; }
[ -n "$VER" ]  || { echo "mklpkg: --version required" >&2; exit 2; }
[ -n "$OUT" ]  || { echo "mklpkg: --out required" >&2; exit 2; }
[ "${#FILES[@]}" -gt 0 ] || { echo "mklpkg: at least one MODE:SRC:DEST file required" >&2; exit 2; }

# Build the header (use printf to keep exact byte layout / LF newlines).
{
    printf 'LPKG1\n'
    printf 'name=%s\n' "$NAME"
    printf 'version=%s\n' "$VER"
    printf 'arch=%s\n' "$ARCH"
    printf 'deps=%s\n' "$DEPS"
    printf 'desc=%s\n' "$DESC"
    printf '%%FILES\n'
    for spec in "${FILES[@]}"; do
        mode="${spec%%:*}"; rest="${spec#*:}"
        src="${rest%%:*}"; dest="${rest#*:}"
        [ -f "$src" ] || { echo "mklpkg: source not found: $src" >&2; exit 1; }
        size="$(stat -c %s "$src")"
        printf '%s %s %s\n' "$mode" "$size" "$dest"
    done
    printf '%%DATA\n'
} > "$OUT"

# Append the raw payload in the same order.
for spec in "${FILES[@]}"; do
    rest="${spec#*:}"; src="${rest%%:*}"
    cat "$src" >> "$OUT"
done

echo "wrote $OUT ($(stat -c %s "$OUT") bytes, ${#FILES[@]} file(s))"
