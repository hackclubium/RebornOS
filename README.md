# RebornOS

A small, serious Unix-like operating system, built from scratch.

Not a Linux clone. Not a microkernel research project. Not a desktop OS.
RebornOS is a UEFI x86_64 OS written primarily in C, with a monolithic
but modular kernel, a small native userspace, and a design that grows
toward POSIX-like behavior over time -- one milestone at a time,
starting from a kernel that boots, logs, and doesn't lie to you when it
crashes.

## Milestone 0: Cold Boot

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

## Milestone 1: Physical Memory

Goal: a physical page allocator built from the UEFI memory map the
bootloader already hands off.

- **`kernel/src/pmm.c`**: a flat bitmap over physical RAM, one bit per
  4 KiB page. Only `EfiConventionalMemory` and `EfiBootServices{Code,Data}`
  regions are treated as allocatable; everything else (including
  firmware's oversized reserved/MMIO windows, which can sit at physical
  addresses with nothing to do with actual installed RAM) stays
  permanently marked used. `pmm_alloc_page()` / `pmm_free_page()` hand
  out and reclaim physical pages.
- Test mode exercises the allocator: distinct pages, independently
  writable, freed pages become reusable again.

## Milestone 2: Preemptive Multitasking (current)

Goal: the kernel owns its own address space, can allocate memory
dynamically, reacts to CPU exceptions and hardware interrupts instead
of triple-faulting blind, and can actually run more than one thing at
once.

- **`kernel/src/vmm.c`**: the kernel's own page tables (PML4/PDPT/PD),
  replacing UEFI's -- identity-mapped, 2 MiB pages, low 4 GiB. The
  first 4 MiB (kernel image) is executable; everything else is NX. Not
  precise page-level W^X (a 2 MiB page's permissions apply to
  everything in it), but a real improvement over the fully-RWX state
  Milestone 0/1 inherited from the linker.
- **`kernel/src/heap.c`**: `kmalloc`/`kfree` over a fixed 16 MiB region
  reserved from the PMM. First-fit free list, forward-only coalescing
  on free (documented, deliberate simplification).
- **`kernel/src/idt.c` + `isr_stubs.S`**: a 256-entry IDT, assembly
  stubs for all 32 CPU exception vectors plus the timer IRQ, funneling
  into one C dispatcher. Exceptions panic with the vector, error code,
  faulting `rip`, and (for page faults) `cr2` -- a crash now produces a
  real diagnosis instead of a silent reset.
- **`kernel/src/timer.c`**: remaps the legacy 8259 PIC off the CPU
  exception vector range and drives a 100 Hz tick via the PIT.
- **`kernel/src/scheduler.c` + `context_switch.S`**: a preemptive
  round-robin scheduler. Context switches save/restore only the
  callee-saved registers and swap `rsp` -- the same mechanism works
  whether switching between two already-running threads (mid-timer-ISR)
  or starting a brand-new one (via a small trampoline that explicitly
  re-enables interrupts, since a fresh thread's first run never goes
  through `iretq`).
- Test mode proves real preemption: two worker threads incrementing
  independent counters, and a third thread that waits for both to pass
  1000 before reporting success -- if the scheduler only ever ran one
  thread, this would hang until the test harness's own timeout.

Not yet: processes, syscalls, a filesystem, or a shell. Those are
later milestones, each starting from this same "boots and can't hide a
bug" foundation.

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
boot/     UEFI bootloader (PE32+, built with mingw)
kernel/   the kernel (ELF64, built with our own cross-compiler)
common/   code shared by both (serial driver, boot_info.h, freestanding libc bits)
tools/    ESP staging, QEMU run/test/debug scripts, panic symbolizer
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
