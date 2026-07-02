# RebornOS

A small, serious Unix-like operating system, built from scratch.

Not a Linux clone. Not a microkernel research project. Not a desktop OS.
RebornOS is a UEFI x86_64 OS written primarily in C, with a monolithic
but modular kernel, a small native userspace, and a design that grows
toward POSIX-like behavior over time -- one milestone at a time,
starting from a kernel that boots, logs, and doesn't lie to you when it
crashes.

## Milestone 0: Cold Boot (current)

Goal: power on in QEMU -> our own hand-written UEFI bootloader finds and
loads the kernel -> the kernel proves it's alive on serial and the
framebuffer -> a real panic path -> fully scriptable dev loop.

- **Bootloader** (`boot/`): a from-scratch UEFI application. No gnu-efi,
  no EDK2 -- the UEFI structs it calls through are hand-transcribed from
  the public UEFI spec in `boot/include/`. It loads `\kernel.elf` from
  the EFI System Partition, parses its ELF64 program headers, calls
  `ExitBootServices`, and jumps into the kernel.
- **Kernel** (`kernel/`): freestanding C17, one assembly entry stub
  (`entry.S`) whose only job is switching onto the kernel's own stack --
  UEFI already leaves the CPU in 64-bit long mode with paging on, so
  there's no mode-transition assembly to write. Brings up a 16550 serial
  driver, a hand-authored 8x8 bitmap font + framebuffer text renderer,
  and a panic path that dumps a return-address stack trace.
- **Debug infra from day one**: `make run` / `make test` / `make debug`.
  Tests are scriptable via QEMU's isa-debug-exit device -- no human
  needs to watch the VM to know if a boot succeeded.

Not yet: memory allocator, interrupts, scheduler, processes, syscalls,
a filesystem, or a shell. Those are later milestones, each starting
from this same "boots and can't hide a bug" foundation.

## Building

Everything here runs inside WSL2 (or native Linux) -- the toolchains
(cross-compiler, mingw, QEMU+OVMF, mtools/sgdisk) are far more
consistent there than on native Windows.

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
    texinfo libisl-dev qemu-system-x86 ovmf mtools gdisk \
    gcc-mingw-w64-x86-64
```

## Layout

```
boot/     UEFI bootloader (PE32+, built with mingw)
kernel/   the kernel (ELF64, built with our own cross-compiler)
common/   code shared by both (serial driver, boot_info.h, freestanding libc bits)
tools/    disk image builder, QEMU run/test/debug scripts, panic symbolizer
toolchain/ builds the x86_64-elf-gcc cross-compiler from source
```

## Philosophy

The kernel provides sharp primitives; userspace builds the actual
world. The roadmap after Milestone 0: physical memory allocator ->
virtual memory / heap -> interrupts & timers -> scheduler -> processes
-> syscalls -> init -> shell -> VFS -> a first filesystem -> executable
loading -> real user programs. GUI, networking, package managers, and
Linux binary compatibility are deliberately out of scope until there's
a command-line system that boots reliably, manages memory correctly,
runs programs, and touches files.
