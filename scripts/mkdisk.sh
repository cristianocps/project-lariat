#!/usr/bin/env bash
#
# mkdisk.sh - create the data/persistence disk images for Project Lariat.
#
# Lariat boots with a volatile ramfs root and mounts two extra IDE disks
# (Unix-style namespace, see docs/adr/0013):
#   * ext4.img  (ext4)  -> /var          : writable, persistent data volume
#                                          (system state /var/etc, package DB, homes)
#   * disk.img  (FAT32) -> /mnt/legacy   : legacy scratch volume (not authoritative)
#
# Disk images are build artifacts (gitignored); recreate them with this script.
# Existing images are left untouched unless --force is given.
#
# Usage: scripts/mkdisk.sh [--force] [--size-mb N]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SIZE_MB=10
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --force) FORCE=1 ;;
        --size-mb) shift; SIZE_MB="$1" ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

need() { command -v "$1" >/dev/null 2>&1 || { echo "error: '$1' not found (install $2)" >&2; exit 1; }; }

make_fat32() {
    local img="$1"
    if [ -f "$img" ] && [ "$FORCE" -ne 1 ]; then
        echo "keeping existing $img (use --force to recreate)"
        return
    fi
    need mkfs.fat dosfstools
    echo "creating $img (${SIZE_MB} MiB, FAT32)"
    dd if=/dev/zero of="$img" bs=1M count="$SIZE_MB" status=none
    # FAT32 requires enough clusters; -F 32 -s 1 keeps small images valid.
    mkfs.fat -F 32 -s 1 "$img" >/dev/null
    # Seed the persistent /etc directory so first boot has a place to write.
    if command -v mmd >/dev/null 2>&1; then
        MTOOLS_SKIP_CHECK=1 mmd -i "$img" ::etc 2>/dev/null || true
    fi
}

make_ext4() {
    local img="$1"
    if [ -f "$img" ] && [ "$FORCE" -ne 1 ]; then
        echo "keeping existing $img (use --force to recreate)"
        return
    fi
    need mkfs.ext4 e2fsprogs
    echo "creating $img (${SIZE_MB} MiB, ext4)"
    dd if=/dev/zero of="$img" bs=1M count="$SIZE_MB" status=none
    # -O ^64bit,^metadata_csum keeps the layout simple for the in-kernel ext4
    # driver (read+write: extents, block/inode bitmap alloc, dir ops).
    # The kernel's m10_setup creates /etc and seeds factory config on first boot.
    mkfs.ext4 -q -F -O ^64bit,^metadata_csum,^huge_file "$img"
}

make_fat32 "$ROOT/disk.img"
make_ext4  "$ROOT/ext4.img"

echo "done. Images: disk.img (FAT32 -> /disk), ext4.img (ext4 -> /ext4)."
