#!/usr/bin/env bash
# Boots build/disk.img under QEMU + OVMF, serial on stdio (interactive).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$DIR/build"

source "$DIR/tools/ovmf-paths.sh"
ensure_ovmf_vars "$BUILD/OVMF_VARS.fd"

# -vga std: our framebuffer code (kernel/src/framebuffer.c) just pokes
# raw pixels straight into the linear buffer GOP already reported --
# there's no Blt/present call anywhere. That matches classic
# VGA/bochs-dispi hardware, where the linear buffer *is* the presented
# surface, but not a virtio-gpu-style adapter (q35's implicit default
# without an explicit -vga flag), which needs an actual flush/present
# command before a raw memory write shows up on screen.
exec qemu-system-x86_64 \
    -machine q35 -m 768M -vga std -display gtk,gl=off \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS.fd" \
    -drive file=fat:16:rw:"$BUILD/esp",format=raw \
    -serial stdio \
    -no-reboot
