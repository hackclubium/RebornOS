#!/usr/bin/env bash
# Boots build/disk.img paused, with a GDB stub on :1234.
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$DIR/build"

source "$DIR/tools/ovmf-paths.sh"
ensure_ovmf_vars "$BUILD/OVMF_VARS.fd"

echo "QEMU is paused waiting for GDB. In another terminal:"
echo "  gdb $BUILD/kernel.elf -ex 'target remote :1234'"
echo

exec qemu-system-x86_64 \
    -machine q35 -m 256M \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS.fd" \
    -drive file=fat:rw:"$BUILD/esp",format=raw \
    -serial stdio \
    -no-reboot \
    -s -S
