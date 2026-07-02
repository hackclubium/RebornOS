#!/usr/bin/env bash
# Resolves raw return addresses from a panic stack trace to function
# names/source lines against the debug-built kernel.elf.
# Usage: tools/symbolize.sh 0xADDR [0xADDR ...]
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_ELF="$DIR/build/kernel.elf"

if [ ! -f "$KERNEL_ELF" ]; then
    echo "ERROR: $KERNEL_ELF not found -- run 'make kernel' first." >&2
    exit 1
fi

if [ "$#" -eq 0 ]; then
    echo "usage: $0 0xADDR [0xADDR ...]" >&2
    exit 1
fi

addr2line -e "$KERNEL_ELF" -f -C "$@"
