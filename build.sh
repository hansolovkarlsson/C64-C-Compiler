#!/bin/sh
# build.sh - compile a .c file straight to a C64 .prg
#
# Usage: ./build.sh source.c [output.prg]
#
# Chains cc64 (C -> assembly) and c64asm (assembly -> .prg) so you don't
# have to invoke them separately. Requires both binaries to be built
# already (cc -std=c99 -O2 -o cc64 cc64.c / cc -std=c99 -O2 -o c64asm c64asm.c),
# and expects them to be in the same directory as this script (or on PATH).

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 source.c [output.prg]" >&2
    exit 1
fi

SRC="$1"
BASE=$(basename "$SRC" .c)
DIR=$(dirname "$SRC")
OUT="${2:-$DIR/$BASE.prg}"
ASM="$DIR/$BASE.asm"

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CC64="$SCRIPT_DIR/bin/cc64"
C64ASM="$SCRIPT_DIR/bin/c64asm"
[ -x "$CC64" ] || CC64=cc64
[ -x "$C64ASM" ] || C64ASM=c64asm

echo "==> compiling $SRC -> $ASM"
"$CC64" tests/"$SRC" -o tests/"$ASM"

echo "==> assembling $ASM -> $OUT"
"$C64ASM" tests/"$ASM" -o tests/"$OUT"

echo "==> done: $OUT"
