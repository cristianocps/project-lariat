#!/usr/bin/env bash
#
# stage-toolchain.sh - stage the native toolchain lpkg packages onto the ext4
# /var image so a booted Lariat can `lpkg install` them.
#
# The toolchain (gcc's cc1 alone is tens of MiB) does not fit in the default
# 10 MiB /var image, so this recreates ext4.img at a larger size and injects the
# packages under /pkgs (which appears on-device as /var/pkgs) via debugfs.
#
# Usage: scripts/stage-toolchain.sh [--size-mb N] [--keep]
#   --size-mb N   size of the recreated ext4 image (default 1024)
#   --keep        do not recreate the image; just (re)write packages into it
#
# On the booted system:
#   lpkg install /var/pkgs/libc-dev-1.2.5.lpkg
#   lpkg install /var/pkgs/binutils-2.42.lpkg
#   lpkg install /var/pkgs/gcc-14.1.0.lpkg
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

IMG="${IMG:-ext4.img}"
PKGDIR="${PKGDIR:-toolchain/packages}"
SIZE_MB=1024
KEEP=0
while [ $# -gt 0 ]; do
    case "$1" in
        --size-mb) shift; SIZE_MB="$1" ;;
        --keep) KEEP=1 ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

DEBUGFS="$(command -v debugfs || echo /usr/sbin/debugfs)"
[ -x "$DEBUGFS" ] || { echo "error: debugfs not found (install e2fsprogs)" >&2; exit 1; }

shopt -s nullglob
pkgs=("$PKGDIR"/*.lpkg)
[ ${#pkgs[@]} -gt 0 ] || { echo "error: no .lpkg in $PKGDIR (run package-sysroot.sh + package-native.sh)" >&2; exit 1; }

if [ "$KEEP" -ne 1 ]; then
    echo "creating $IMG (${SIZE_MB} MiB, ext4)"
    dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
    mkfs.ext4 -q -F -O ^64bit,^metadata_csum,^huge_file "$IMG"
fi

"$DEBUGFS" -w -R "mkdir /pkgs" "$IMG" >/dev/null 2>&1 || true
for p in "${pkgs[@]}"; do
    base="$(basename "$p")"
    echo "  staging $base ($(du -h "$p" | cut -f1))"
    "$DEBUGFS" -w -R "rm /pkgs/$base" "$IMG" >/dev/null 2>&1 || true
    "$DEBUGFS" -w -R "write $p /pkgs/$base" "$IMG" >/dev/null 2>&1
done

echo "staged ${#pkgs[@]} package(s) into $IMG:/pkgs"
"$DEBUGFS" -R "ls -l /pkgs" "$IMG" 2>/dev/null || true
