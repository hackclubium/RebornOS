# RebornOS

![CI](https://github.com/hackclubium/RebornOS/actions/workflows/ci.yml/badge.svg)
![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)
![Language](https://img.shields.io/badge/language-C17-blue.svg)
![Platform](https://img.shields.io/badge/platform-UEFI%20x86--64-lightgrey.svg)

A small, serious Unix-like operating system, built from scratch.

Not a Linux clone. Not a microkernel research project. Not a desktop OS.
RebornOS is a UEFI x86_64 OS written primarily in C, with a monolithic
but modular kernel, a small native userspace, and a design that grows
toward POSIX-like behavior over time, starting from a kernel that boots,
logs, and doesn't lie to you when it crashes.

## Features

- **From-scratch UEFI bootloader** -- no gnu-efi, no EDK2. Loads and
  parses the kernel's own ELF64 image directly off the EFI System
  Partition.
- **Real memory management** -- a physical page allocator, the kernel's
  own page tables, a heap, per-process address spaces, and
  page-fault-driven stack growth.
- **Preemptive multitasking** -- a round-robin scheduler, ring 0/ring 3
  privilege separation, and a real `int $0x80` syscall boundary.
- **Process isolation** -- every process gets its own page tables, with
  real per-process pointer validation instead of a heuristic.
- **A real filesystem and disk driver** -- FAT16 with subdirectories,
  arguments, and write support, served by a polled AHCI driver (no
  RAM-loaded disk images).
- **An interactive shell** -- PS/2 keyboard and mouse drivers, disk-loaded
  ELF programs, and a real shell to launch them.
- **Networking** -- a polled Intel e1000 driver plus Ethernet/ARP/ICMP,
  proven with a real ping round-trip through QEMU's NAT.
- **SMP** -- every CPU core discovered via ACPI and brought up through a
  real-mode trampoline into 64-bit long mode.
- **Fault isolation** -- a crashing user process is killed and reported,
  not the whole kernel.
- **Graphics primitives** -- framebuffer mapping, mouse input, and a
  heap-growth syscall for userland, laying the groundwork for a GUI.

Not yet: a windowing system, UDP/TCP, piping, package management, or
Linux binary compatibility. GUI and userland ergonomics are deliberately
on hold until the kernel itself is rock solid.

## Building

Everything here runs inside WSL2 (or native Linux) -- the toolchains
(cross-compiler, mingw, QEMU+OVMF) are far more consistent there than
on native Windows.

```sh
# one-time: build our own x86_64-elf-gcc cross-compiler into ~/opt/cross
make toolchain

# boot it interactively (serial on stdio, Ctrl-A X to quit QEMU)
make run

# scriptable pass/fail smoke test (no human watching QEMU)
make test

# boot paused with a GDB stub on :1234
make debug
```

Required host packages (Ubuntu/Debian):

```sh
sudo apt-get install -y bison flex libgmp-dev libmpc-dev libmpfr-dev \
    texinfo libisl-dev qemu-system-x86 ovmf gcc-mingw-w64-x86-64
```

## Layout

```
boot/      UEFI bootloader (PE32+, built with mingw)
kernel/    the kernel (ELF64, built with our own cross-compiler)
common/    code shared by both (serial driver, boot_info.h, freestanding libc bits)
userland/  RebornOS's own user programs (ELF64, no libc, loaded off disk at boot)
tools/     ESP staging, QEMU run/test/debug scripts, panic symbolizer
toolchain/ builds the x86_64-elf-gcc cross-compiler from source
```

## Philosophy

The kernel provides sharp primitives; userspace builds the actual
world. GUI, package managers, and Linux binary compatibility are
deliberately out of scope until there's a command-line system that
boots reliably, manages memory correctly, runs isolated programs, and
touches files.

## License

MIT -- see [LICENSE](LICENSE).
