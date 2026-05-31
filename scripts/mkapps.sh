#!/usr/bin/env bash
#
# mkapps.sh - package the Phase 3 reference GUI apps as .lpkg files.
#
# Builds installable packages for the calculator, editor, and terminal clients
# into dist/packages/.  Install them inside Lariat with:
#
#     lpkg install /var/packages/calc-1.0.0.lpkg
#
# (copy dist/packages/*.lpkg onto the /var data volume, or fetch via `lpkg
# install-net` from an HTTP repository).
#
# Usage: scripts/mkapps.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT="dist/packages"
mkdir -p "$OUT"

# Ensure the app ELFs are built.
make -C userspace calc.elf edit.elf term.elf >/dev/null

pack() {
    local name="$1" ver="$2" desc="$3" elf="$4" dest="$5"
    scripts/mklpkg.sh --name "$name" --version "$ver" --arch x86_64 \
        --desc "$desc" --out "$OUT/$name-$ver.lpkg" \
        "0755:$elf:$dest"
}

pack calc 1.0.0 "GUI calculator"   userspace/calc.elf usr/bin/calc
pack edit 1.0.0 "GUI text editor"  userspace/edit.elf usr/bin/edit
pack term 1.0.0 "GUI terminal"     userspace/term.elf usr/bin/term

echo "packages in $OUT:"
ls -1 "$OUT"
