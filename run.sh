#!/bin/sh
# run.sh - run a .prg file
#
# Usage: ./run.sh program.prg
#
# Starts VICE C64 mode, silencing output and sends errors to vice.log (VICE sends them to stdout)
#

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 program.prg" >&2
    exit 1
fi

PRG="./tests/$1"
DIR=$(dirname "$PRG")

/Applications/vice-arm64-gtk3-3.10/bin/x64sc -silent "$PRG" > vice.log &

