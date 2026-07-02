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

## Milestone 2: Preemptive Multitasking

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

## Milestone 3: User Mode + Syscalls

Goal: the CPU actually drops to ring 3, user code can ask the kernel to
do things on its behalf through a real syscall boundary, and the
kernel gets back control safely when it's done.

- **`kernel/src/gdt.c` + `gdt_asm.S`**: RebornOS's own GDT (replacing
  UEFI's, same as Milestone 2 replaced UEFI's page tables) with kernel
  and user code/data segments, plus a 64-bit TSS whose `RSP0` is the
  kernel stack the CPU switches to on any ring3->ring0 transition.
  `enter_usermode()` builds a fake interrupt-return frame by hand and
  executes `iretq` -- the only way to drop CPL going down.
- **`int $0x80`**: a syscall vector reusing the exact same
  `isr_common_stub`/`interrupt_frame_t` machinery every CPU exception
  and the timer IRQ already go through, just with a DPL=3 IDT gate so
  ring 3 is actually allowed to invoke it. `SYS_WRITE` and `SYS_EXIT`
  are implemented in `kernel/src/syscall.c`.
- **No per-process address space yet**: user and kernel code share the
  same page tables (now marked user-accessible in `vmm.c`), so this
  milestone proves the *mechanism* -- privilege transitions, the
  syscall boundary, TSS-driven stack switching -- without real process
  isolation. A user pointer handed to a syscall is trusted directly,
  which is fine today but would need validation the moment separate
  address spaces exist. Real isolation is a later milestone.
- Test mode runs a ring-3 program (still linked into the kernel image
  -- no filesystem/ELF loader for user binaries yet) that calls
  `SYS_WRITE` five times over `int $0x80` and then `SYS_EXIT`s,
  interleaved with the Milestone 2 worker threads. The monitor thread
  requires all three to make progress before reporting success.
- One real gotcha this milestone hit: a fresh thread's first "exit" by
  looping on `hlt` inside the syscall handler would freeze the *entire
  system* forever, not just that thread -- the handler runs with
  interrupts disabled (interrupt gate) and never returns via `iretq`,
  so nothing could ever wake it. Since the scheduler has no way to
  remove a thread from the round-robin yet, `SYS_EXIT` instead makes
  the thread voluntarily yield forever, letting everything else keep
  running.

(Serial output from different threads can interleave mid-line in the
logs -- `kprintf` has no locking across threads yet. Cosmetic only;
a real fix is a lock/spinlock primitive, itself a future milestone.)

## Milestone 4: Process Isolation (current)

Goal: separate processes actually get separate memory. Milestone 3
proved the privilege-transition mechanism; this proves the isolation
that mechanism exists for in the first place.

- **`vmm_create_address_space()`** (`kernel/src/vmm.c`): every address
  space's `PML4[0]` points at the exact same shared PDPT Milestone 2
  built -- kernel code, the heap, the framebuffer, all identical for
  every process, since kernel code has to stay reachable across any
  privilege transition no matter which process is running. What makes
  two processes actually different is `PML4[1]`: a freshly built,
  process-private PDPT/PD/PT chain mapping exactly one page at a fixed
  virtual address (`PROCESS_PRIVATE_VADDR`, 0x8000000000). Two
  processes using that identical virtual address end up backed by two
  different physical pages -- deliberately minimal (one page, not a
  general-purpose heap/stack per process), but a real isolation
  boundary rather than a shared one.
- **CR3 per thread**: `scheduler.c`'s thread table now carries a `cr3`,
  loaded on every context switch (`thread_create` uses the kernel's
  address space; `thread_create_process` uses a fresh one from
  `vmm_create_address_space()`).
- **Syscall pointer validation** (`kernel/src/syscall.c`): `SYS_WRITE`
  now bounds-checks the pointer it's given before dereferencing it,
  panicking with a clear message instead of an unrelated page fault
  three calls deep in `kprintf`. Documented as necessary-but-not-
  sufficient: every process maps its private page at the *same*
  virtual address, so a syscall still can't distinguish "this
  process's page" from "some other process's page" without real
  per-process access tracking, which doesn't exist yet.
- Test mode runs two ring-3 processes that each repeatedly write a
  distinct byte pattern to `PROCESS_PRIVATE_VADDR` and immediately read
  it back, self-reporting failure over syscall if they ever observe the
  other's value -- interleaved with the Milestone 2/3 checks.
- The real bug this milestone caught: `TSS.RSP0` is *the* kernel stack
  the CPU switches to on any ring3->ring0 transition -- there's only
  one such register. With a single ring-3 thread (Milestone 3) a fixed
  shared value was harmless. With three ring-3-capable threads, one
  thread's `SYS_EXIT` yield-loop saved its paused state pointing into
  that shared stack, and the next thread's syscall promptly overwrote
  that exact memory before the first thread was ever resumed -- silent
  corruption, not a crash, manifesting as one thread mysteriously
  stalling after its first syscall while everything else kept running.
  Fixed by giving every thread its own kernel stack and having the
  scheduler update `TSS.RSP0` on every switch, alongside `CR3`.

Not yet: a filesystem or a shell, so a "process" is still a function
baked into the kernel image, not something loaded from disk. Those are
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
world. Roadmap so far: physical memory allocator -> virtual memory /
heap -> interrupts & timers -> scheduler -> user mode & syscalls ->
per-process address space isolation (done). Next: init -> a shell ->
VFS -> a first filesystem -> loading real user programs from disk. GUI,
networking, package managers, and Linux binary compatibility are
deliberately out of scope until there's a command-line system that
boots reliably, manages memory correctly, runs isolated programs, and
touches files.
