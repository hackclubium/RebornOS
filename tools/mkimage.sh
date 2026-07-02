#!/usr/bin/env bash
# Stages the bootloader, kernel, and userland program into a directory
# laid out like an EFI System Partition.
# Usage: mkimage.sh <esp-dir> <BOOTX64.EFI> <kernel.elf> <init.elf>
#
# We don't build an actual FAT disk image here. QEMU's built-in VVFAT
# driver (`-drive file=fat:16:rw:<dir>`, forced to FAT16 so the kernel's
# own FAT16 driver has a guaranteed format to parse -- vvfat's FAT32
# mode is explicitly documented by QEMU itself as untested, and really
# is broken: it produces a volume OVMF's own boot manager can't find)
# synthesizes a spec-compliant FAT filesystem on the fly from a host
# directory, which is the standard recipe for UEFI development in QEMU
# -- it sidesteps a real gotcha with hand-building FAT images via
# mtools/mformat, whose automatic cluster-size heuristic can produce a
# filesystem that Linux reads fine but that EDK2's (OVMF's) strict FAT
# driver silently refuses to mount. Real hardware deployment (a proper
# GPT + ESP raw image) is a separate concern for a later milestone.
set -euo pipefail

ESP_DIR="$1"
BOOTLOADER="$2"
KERNEL="$3"
USERPROG="$4"

rm -rf "$ESP_DIR"
mkdir -p "$ESP_DIR/EFI/BOOT"
cp "$BOOTLOADER" "$ESP_DIR/EFI/BOOT/BOOTX64.EFI"
cp "$KERNEL" "$ESP_DIR/kernel.elf"
cp "$USERPROG" "$ESP_DIR/INIT.ELF"

echo "staged ESP directory at $ESP_DIR"
