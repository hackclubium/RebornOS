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

## Milestone 4: Process Isolation

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

## Milestone 5: Disk-Loaded Programs

Goal: a "process" stops being a C function compiled into the kernel's
own binary and becomes an actual file, found and loaded off a real
filesystem the way a real OS starts any program.

- **The bootloader now reads the whole ESP volume into RAM**
  (`boot/src/main.c`) via `EFI_BLOCK_IO_PROTOCOL` on the same device
  handle `SimpleFileSystem` already used to find `kernel.elf` -- for a
  FAT partition, that handle's own block space *is* the FAT volume's
  LBA space, so no MBR/GPT parsing is needed. There's no disk driver in
  the kernel yet (AHCI/NVMe are real hardware-driver work, a separate
  future milestone), so this in-RAM copy, handed off via two new
  `boot_info_t` fields, is how the kernel gets bytes to parse.
- **`kernel/src/fat16.c`**: a read-only FAT16 driver over that in-RAM
  image -- BPB parsing, FAT cluster-chain walking, 8.3 filename lookup
  in the root directory. FAT16, not FAT32: QEMU's own `vvfat` driver
  documents its FAT32 support as untested, and it really is broken here
  (forcing it produced a volume OVMF's own boot manager couldn't even
  find). FAT16 turned out simpler anyway -- its root directory is a
  fixed-size area right after the FATs, not a cluster chain.
- **`kernel/src/vfs.c`**: the thinnest possible VFS. Exactly one
  filesystem is ever mounted, so `vfs_read_file(path)` just forwards to
  the FAT16 driver and returns a `kmalloc`'d buffer with the whole file
  -- no mount table, no directories, no incremental reads. A real VFS is
  future work once there's more than one filesystem to route between.
- **`vmm_map_page()`** (`kernel/src/vmm.c`): a general-purpose 4 KiB
  page mapper, unlike Milestone 4's `vmm_create_address_space()` which
  only ever carves out one fixed private page. Walks/allocates
  PML4->PDPT->PD->PT for an arbitrary virtual address, guarded against
  ever touching `PML4[0]` -- the single shared low-4GiB mapping every
  process's page tables point at verbatim, which this must never write
  into, or one process's program would corrupt every other process's
  view of the kernel.
- **`kernel/src/elf_loader.c`**: parses a static ELF64 executable's
  `PT_LOAD` program headers, allocates a physical page per page of each
  segment, copies in the file-backed bytes (zeroing the rest for bss),
  and maps each one into the target process's own address space with
  per-segment permissions (writable data, executable text, neither for
  read-only data) via `vmm_map_page()`. Also sets up a small dedicated
  user stack. This -- not the fixed fake private page from Milestone 4
  -- is what a real process's memory layout looks like.
- **`userland/init.c`**: RebornOS's first genuinely separate program.
  Built with the same cross-compiler, linked to run at a fixed address
  inside its own private address-space range (never inside the shared
  low-4GiB region), staged onto the ESP as `INIT.ELF`, and loaded by
  the kernel at boot the same way `thread_create_process()` already
  loaded the Milestone 4 demo processes -- except this one came from a
  file, not from `kmain.c`. No libc, no crt0: `enter_usermode()` already
  leaves the stack set up, so its entry point is just an ordinary C
  function using the same raw `int $0x80` syscalls as every other ring-3
  test program.
- Two real bugs this milestone caught, both in existing infrastructure
  the new code just happened to be the first thing to exercise hard
  enough:
  - **`panic()` wasn't safe to call from inside a fault.** `%rbp` is not
    reloaded on a ring3->ring0 transition, so it can hold whatever a
    user program last left in it; `print_stack_trace()` walking that
    value can itself fault, re-entering `panic()` from inside
    `exception_handler()`, which tries the exact same unsafe printing
    again -- an unbounded fault storm instead of a clean exit. Fixed
    with a re-entrancy guard: the second entry skips straight to
    `qemu_debug_exit()`.
  - **Syscall pointer validation didn't know about loaded programs.**
    `user_ptr_valid()`'s whitelist only covered the shared low-4GiB
    region and Milestone 4's one fixed demo page, so it correctly (by
    its own rules) rejected `INIT.ELF`'s perfectly valid `.rodata`
    string pointer. Extended with a third range covering every loaded
    program's fixed load base through its stack top -- exactly as
    "necessary but not sufficient" as the check it sits next to, since
    there's still no true per-process access tracking.
- Test mode now also loads and runs `INIT.ELF` for real, alongside the
  Milestone 2-4 checks, and asserts on a minimum `SYS_WRITE` count that
  only `INIT.ELF` and the fixed `ring3_program` can guarantee
  deterministically (Milestone 4's two isolation processes race their
  own finish-message writes against the monitor's check).

Not yet: subdirectories, writing to disk, a real disk driver (this
still reads the whole volume into RAM via UEFI's `BLOCK_IO` before
`ExitBootServices` -- fine for a small hand-built ESP, not how a real
OS talks to a real disk), or a shell to launch programs interactively
instead of `kmain.c` hardcoding which ones to run. Those are next.

## Milestone 6: Interactive Shell

Goal: a human can actually type at RebornOS. Every milestone before
this one was fully scripted -- the kernel deciding on its own what to
run, when -- because there was no input device at all. This is the
first time that changes.

- **`kernel/src/keyboard.c`**: a PS/2 keyboard driver. IRQ1, US QWERTY,
  scancode set 1 (what the i8042 controller normalizes every keyboard
  down to). Lowercase only -- Shift/Ctrl/Alt are recognized just enough
  to be ignored rather than corrupting the output, which is enough to
  type a filename (`fat16.c` uppercases everything when matching
  anyway) and run `ls`; real shifted/symbol input is future work.
  Required generalizing `idt_set_irq_handler()` from a single global
  callback (only the timer ever used it) into a small table indexed by
  IRQ line, and exposing the PIC's EOI/unmask primitives from
  `timer.c` (which already owned PIC init from Milestone 2) so a second
  device can use them too.
- **Real thread exit** (`kernel/src/scheduler.c`): every process before
  this milestone "exited" by yielding forever in an infinite loop --
  documented since Milestone 3 as a stopgap because the scheduler had
  no way to actually remove a thread. `thread_exit()` now really does:
  each thread slot gets a state (`ACTIVE`/`ZOMBIE`/`UNUSED`), and
  `schedule()` frees a zombie's stack and frees its slot for reuse right
  after switching away from it. `SYS_EXIT` calls it instead of looping.
- **`kernel/src/process.c`**: `process_spawn()` and
  `process_spawn_and_wait()` tie the VFS, ELF loader, and scheduler
  together into "run this program from disk," used by booting the
  shell, the new `SYS_EXEC` syscall, and a self-test. Waiting for a
  spawned program needs no new synchronization primitive -- it's just a
  loop checking `thread_is_alive()` and calling `schedule()`, the same
  cooperative round-robin every thread already participates in.
- **Three new syscalls** (`kernel/src/syscall.c`): `SYS_READ_CHAR`
  (blocks -- cooperatively -- until a keystroke is available),
  `SYS_EXEC` (loads a program by name and blocks until it exits, so a
  shell doesn't need its own wait/fork logic), and `SYS_LIST_ROOT`
  (fills a buffer with the root directory's filenames, for `ls`).
- **`userland/shell.c`**: RebornOS's actual shell. A real ELF64
  executable, loaded off disk exactly like `INIT.ELF`, no libc. Prints
  a prompt, reads a line with backspace handling and per-character
  echo, treats `ls` as a builtin, and hands anything else to `SYS_EXEC`
  as a program name.
- The Makefile's userland section now builds a *list* of programs
  (`init`, `shell`), each its own independently linked ELF staged onto
  the ESP as its own uppercased 8.3 name -- adding a new userland
  program going forward means one line in `Makefile` and one `.c` file.
- A real, if narrow, concurrency bug this milestone caught: `thread_exit()`
  used to `sti` (re-enable interrupts) right before yielding away for
  the last time, so that the system wouldn't stay permanently
  interrupt-disabled if every other thread was cooperatively yielding
  instead of relying on the timer. But that opened a window: if a timer
  IRQ landed between `schedule()` freeing the exiting thread's stack and
  actually switching away from it, the IRQ handler would recursively
  re-enter `schedule()` *on top of that same about-to-be-abandoned
  stack*, corrupting whatever thread got switched away from inside that
  nested call -- surfacing as a `make test` failure roughly one run in
  six, always an Invalid Opcode fault at some near-zero garbage `rip`.
  Fixed by making `schedule()`'s whole body atomic (`cli` on entry)
  instead of trying to keep interrupts enabled across it.
- Test mode covers the new pieces without needing a real keystroke:
  `keyboard_inject_char()` feeds the ring buffer directly to test the
  buffer + translation path, and a plain kernel thread calls
  `process_spawn_and_wait()` on `INIT.ELF` to exercise the exact
  mechanism `SYS_EXEC` uses (and to double as a `thread_exit()`
  exercise) -- both deterministic, neither needing a human at the
  keyboard. The real shell, with real typed input, is verified via
  `make run`.

Not yet: Shift/symbol input, command history, arguments (a shell
command is just a bare program name -- no `ls -l`-style parsing),
piping/redirection, or running more than one foreground program at a
time. Those, plus real disk drivers and subdirectories from Milestone
5's list, are all still ahead.

## Milestone 7: A Real Disk Driver

Goal: stop faking it. Every milestone since the filesystem showed up
has been reading a one-time copy of the *entire* boot volume that the
bootloader slurped into RAM before `ExitBootServices` -- convenient,
but not how an OS actually talks to a disk. This milestone deletes that
hack and gives the kernel a real AHCI driver instead.

- **`kernel/src/pci.c`**: legacy PCI configuration space access
  (ports 0xCF8/0xCFC -- works on every x86 PC including QEMU's q35, no
  ACPI MCFG/ECAM parsing needed for a plain config-space read) and
  `pci_find_device()`, scanning bus 0's devices/functions for the one
  matching a given class/subclass/prog-if. Needed 32-bit port I/O
  (`outl`/`inl`), which `common/include/ioport.h` didn't have yet --
  every prior driver in this kernel only ever needed single bytes.
- **`kernel/src/ahci.c`**: a minimal polled AHCI driver implementing
  `blockdev.h`'s two functions. Finds the ICH9 AHCI controller via
  `pci.c`, enables memory space + bus mastering, maps ABAR, finds the
  first port with a SATA disk actually attached (`PxSSTS`/`PxSIG`), and
  reprograms its command list and FIS receive area (one page each --
  a physical page from `pmm_alloc_page()` trivially satisfies AHCI's
  1KiB/256B alignment requirements, so no dedicated aligned allocator
  was needed). Issues `READ DMA EXT` on a single command slot and polls
  `PxCI` for completion -- no interrupts, no NCQ, no write support, no
  BIOS/OS handoff or full HBA reset (OVMF's own AHCI driver just used
  this exact controller to load our kernel, so it's already in a sane
  state). This targets QEMU's emulated controller specifically; real
  hardware may need more care.
- **`kernel/src/fat16.c` rewritten to read through `blockdev.h`**
  instead of indexing into a RAM array. The whole FAT gets cached in
  RAM once at init (small -- FAT16 tops out around 128KiB -- so cluster
  chain walks don't cost a disk read per step), the root directory is
  read into a temporary buffer per lookup, and file data is read one
  cluster at a time straight into the caller's buffer.
- **A real wrinkle from going through actual disk hardware**: reading
  the boot sector's LBA 0 via AHCI didn't produce a FAT16 BPB at all --
  it produced an MBR. The UEFI `BLOCK_IO` handle Milestone 5 used was
  scoped to the *partition* SimpleFileSystem had already found, so its
  LBA 0 was implicitly the volume's own boot sector; going through the
  disk controller directly means LBA 0 is the *whole disk's* first
  sector, and QEMU's vvfat driver in `:rw:` mode partitions that disk
  (unlike its floppy mode, which is a superfloppy with no partition
  table). Fixed by having `fat16_init()` check whether LBA 0 actually
  looks like a FAT16 BPB, and if it doesn't, parse it as an MBR instead
  and follow the first partition table entry to find where the real
  volume starts.
- **The bootloader's whole-disk RAM copy is gone**: `boot/src/main.c`
  no longer touches `EFI_BLOCK_IO_PROTOCOL` at all, `boot_info_t` lost
  its `disk_image_addr`/`disk_image_size` fields, and the QEMU memory
  bump Milestone 5 needed (256M -> 768M, since vvfat always synthesizes
  a ~504 MiB disk regardless of how little the host directory contains)
  reverts back down to 256M now that nothing ever loads that whole
  volume into guest RAM at once.
- Existing test-mode coverage (loading `INIT.ELF`, booting `SHELL.ELF`)
  now exercises the real AHCI path with no changes needed -- if the
  disk driver were broken, everything downstream would immediately
  fail instead of needing a dedicated synthetic test.

Not yet: NCQ/interrupt-driven completion, more than one port/disk, GPT
(only a legacy MBR's first partition entry is read), or support for any
AHCI controller beyond QEMU's emulated ICH9. Real hardware
compatibility (spin-up delays, BIOS/OS handoff) is unexplored. Write
support landed the very next milestone.

## Milestone 8: Subdirectories, Arguments, and Write Support

Goal: three separate upgrades that all make the shell feel like a real
one instead of a demo -- programs can live in folders, take arguments,
and the disk stops being read-only.

- **Subdirectories** (`kernel/src/fat16.c`): `fat16_open()` and the
  renamed `fat16_list_dir()` now resolve multi-component paths like
  `DEMO/HELLO.ELF`. Each non-final path component gets looked up as a
  directory and its cluster chain read as a raw array of directory
  entries (a subdirectory's total size isn't stored anywhere the way a
  file's is, so this walks the chain once to count clusters, then again
  to read them) -- everything else about scanning for an 8.3 name is
  shared with the existing root-directory lookup via one
  `find_entry_in_dir()` helper. Writing is still root-only (see below).
- **Arguments**: real argv, not just a bare program name. `iretq`
  doesn't touch general-purpose registers, so `enter_usermode()`
  (`kernel/src/gdt_asm.S`) can set `rdi`/`rsi` to argc/argv right before
  executing it and the new process receives them exactly like an
  ordinary SysV call -- no crt0 startup shim needed.
  `elf_load_user_program()` packs the argument strings plus a
  NULL-terminated pointer array into the top of the stack page it
  already allocates for every process, nudging the returned stack
  pointer down below the blob so the process's own stack usage grows
  away from it instead of into it. `SYS_EXEC`'s ABI changed from a bare
  program-name pointer to `(argv, argc)`, matching `execv()`.
  `userland/echo.c` is a new tiny program that exists purely to print
  back whatever arguments it's given, proving the whole chain works.
- **Write support** (`fat16_write_file()`, root directory only --
  writing into a subdirectory would need growing its cluster chain,
  deferred): allocates free clusters from the cached FAT, frees the old
  chain first when overwriting an existing file, writes the data via a
  new `blockdev_write_sectors()` (WRITE DMA EXT, symmetric with the
  read side added last milestone), then writes back just the changed
  directory-entry sector and the whole FAT cache.
- **Three new/changed syscalls**: `SYS_LIST_DIR` (renamed from
  `SYS_LIST_ROOT`, now takes a path), `SYS_READ_FILE`, and
  `SYS_WRITE_FILE`. The shell gained matching builtins -- `ls [path]`,
  `cat <file>`, `write <file> [text...]` -- plus a line tokenizer so
  typing `echo hello world` actually splits into an argv SYS_EXEC can
  use instead of treating the whole line as one program name.
- **A real, if narrow, validation bug this milestone caught**: the
  argument string SYS_EXEC's own path takes gets placed close to the
  very top of the child's stack (that's where the argv blob lives), and
  `user_ptr_valid()`'s check for `SYS_WRITE` conservatively required
  256 bytes of headroom past the pointer to account for a
  worst-case-length string -- which, for a short argv string sitting
  near `ELF_USER_STACK_TOP`, "spilled" past that boundary on paper even
  though the actual NUL-terminated read would stop well short of it.
  Fixed by only requiring the pointer's *start* address to be in range
  for that check, not the full conservative length: `kprintf`'s `%s`
  already stops at the real NUL, and a compiler-placed buffer can't
  itself exceed the stack it was allocated in.
- Test mode gained three deterministic self-tests: a closed-loop
  write-then-read-back-and-compare check (no eyeballing the serial log
  needed), a subdirectory exec test (`DEMO/HELLO.ELF`, a copy of
  `INIT.ELF` mkimage.sh stages into a real subdirectory purely as a
  test fixture -- there's no `mkdir` yet), and an argv exec test
  (`ECHO.ELF hello world`).

Not yet: piping (`cmd1 | cmd2`) -- deliberately cut from this push. A
real pipe needs a file-descriptor/stdout-redirection abstraction that
doesn't exist anywhere in the kernel yet (`SYS_WRITE` always writes
straight to the serial console); bolting that on as an afterthought
here would have been worse than giving it its own milestone. Also not
yet: quoting in the shell's tokenizer, command history, writing into
subdirectories, and growing a directory's cluster chain when it's full.

## Milestone 9: Real Virtual Memory

Goal: close the gap between "process isolation" as a demo and process
isolation as something actually enforced. Every process has had its own
page tables since Milestone 4, but the low 4 GiB every address space
shares (kernel code, the heap, the framebuffer, the page tables
themselves) was mapped `PAGE_USER` end to end -- a ring-3 program could
dereference any address below 4 GiB and the CPU would allow it. This
milestone makes that mapping genuinely ring-0-only, replaces the
syscall layer's coarse pointer-range heuristic with real per-process
page-table validation, and adds page-fault-driven stack growth so the
user stack isn't a fixed, non-growable allocation anymore.

- **The shared low-4GiB mapping is no longer user-accessible**
  (`kernel/src/vmm.c`): only the executable first 4 MiB keeps
  `PAGE_USER` (see the next bullet for why), everything else -- the
  heap, the physical page bitmap, the page tables themselves, the
  framebuffer -- is supervisor-only. Ring 0 can still reach all of it
  regardless (the User bit only gates ring-3 accesses), but a ring-3
  program's own page tables share this exact mapping verbatim, so this
  is what actually makes kernel memory off-limits to user code rather
  than just conventionally left alone.
- **A real compatibility snag from tightening that**: `kmain.c` has had
  a pair of hand-rolled ring-3 test programs (`ring3_program`,
  `process_body`) since before the ELF loader existed, launched
  directly out of the kernel's own low-memory text section -- which is
  exactly why the executable first 4 MiB above keeps `PAGE_USER`. Their
  *stacks*, though, were plain `kmalloc()` buffers from the now
  correctly non-user kernel heap, so the very first thing they did
  (push a return address) turned into an instant ring-3 page fault.
  Fixed by giving them a real per-process stack instead -- allocated
  via `pmm_alloc_page()` and mapped into the calling thread's own
  address space (`vmm_current_cr3()`, always available since a syscall
  or a fresh thread's first instructions never switch CR3 out from
  under it) the same way `elf_loader.c` already builds a stack for a
  real loaded program.
- **Real per-process pointer validation** (`vmm_check_range()`,
  `vmm.c`): walks the caller's actual page tables to confirm a
  syscall's user-supplied buffer is really present, user-accessible,
  and (when the kernel intends to write into it) writable, instead of
  the old `user_ptr_valid()`'s "is this address in one of a few known
  ranges" heuristic. Works for any buffer a process legitimately has
  mapped, not just hardcoded regions. `copy_user_string()` now
  re-validates one page at a time as it copies a NUL-terminated string
  out of user memory, rather than requiring the whole worst-case length
  to be valid up front -- which also fully retires the awkward
  "only the start address needs to be in range" workaround Milestone 8
  needed for the exact same check.
- **Page-fault-driven stack growth** (`elf_handle_stack_fault()`,
  `elf_loader.c`): the ELF loader now only maps a user program's
  topmost stack page eagerly; a not-present write fault from ring 3
  anywhere else within a bounded growth region (`ELF_USER_STACK_MAX_PAGES`,
  256 KiB) demand-maps a fresh zeroed page and retries the faulting
  instruction, instead of the old fixed 16 KiB allocation that could
  never grow. A fault outside that range, from ring 0, or on an
  already-present page still falls through to `idt.c`'s normal panic
  path -- this only ever papers over the one specific, legitimate case.
  `userland/stacktst.c` is a new test program that recurses far past
  the first page specifically to prove this is really happening rather
  than just never being exercised.
- Test mode gained a deterministic self-test (`stack-test`) spawning
  `STACKTST.ELF` and asserting it exits cleanly, plus every existing
  self-test now runs under the tightened permissions unchanged.

Not yet: demand-paged heap growth for user programs (none of today's
programs need one), copy-on-write, swapping, or any notion of memory
outside a process's fixed load region and its one growable stack.

## Milestone 10: Networking

Goal: RebornOS talks to the outside world. A polled Intel 82540EM
("e1000") driver -- QEMU's default emulated NIC -- plus just enough of
a network stack (Ethernet framing, ARP, ICMP echo) to send and receive
real packets through QEMU's user-mode (SLIRP) NAT, proven by actually
pinging the gateway and getting a real reply back.

- **`kernel/src/e1000.c`**: same shape as `ahci.c` -- finds the card via
  PCI (class 0x02 network, subclass 0x00 ethernet), enables memory
  space + bus mastering, maps BAR0, resets the device, and reads our
  own MAC straight back out of `RAL0`/`RAH0` (QEMU pre-populates those
  from `-device e1000,mac=...`, so no EEPROM read protocol was needed).
  Builds a small 8-descriptor RX ring and 8-descriptor TX ring (one
  physical page each, trivially satisfying the 128-byte alignment
  requirement -- same reasoning as `ahci.c`'s command structures),
  masks every interrupt (this driver is polled, like the disk driver),
  and exposes three functions: `e1000_send()` (blocks on the TX
  descriptor's Descriptor Done bit), `e1000_poll_receive()`
  (non-blocking, returns 0 if nothing's arrived), and
  `e1000_get_mac()`.
- **`kernel/src/net.c`**: the thinnest stack that could prove this
  works -- Ethernet framing, ARP request/reply, and ICMP echo, nothing
  else. No DHCP: our own IP is a hardcoded constant matching what QEMU's
  SLIRP network would hand out anyway (`10.0.2.15`, gateway `10.0.2.2`),
  since implementing a DHCP client purely to arrive at the same fixed
  value the test network always uses wasn't worth it yet.
  `net_arp_resolve()` and `net_ping()` both retry a bounded number of
  times while polling for a reply and return 0 on timeout rather than
  panicking -- a dropped frame or slow reply is a normal, expected
  condition here, not a kernel bug; the caller decides whether that
  failure matters.
- **A new self-test** (`network_test_thread` in `kmain.c`) that ARPs for
  the gateway's MAC, then sends it a real ICMP echo request and checks
  for a matching reply (same identifier, sequence, and payload) --
  QEMU's SLIRP gateway answers both like a real host would, so this
  only passes if frames genuinely left the card, got NATed, and the
  driver's RX path delivered the reply back.
- **`memcmp` added to `common/src/minilib.c`**: net.c's reply-matching
  needed real buffer comparison, which this freestanding libc didn't
  have yet (only `memset`/`memcpy`/`memmove`/`strlen`/`strcmp`
  existed).
- All three QEMU scripts (`run-qemu.sh`, `test-qemu.sh`,
  `debug-qemu.sh`) gained `-netdev user,id=net0 -device e1000,...` --
  SLIRP's NAT needs no host root privileges or tap device setup, so
  `make test` stays a fully scriptable, no-special-privileges command.

Not yet: DHCP, UDP/TCP (so no real sockets or an HTTP server yet --
that's the natural next step once there's a reason to need a transport
protocol), IPv6, and anything beyond a single hardcoded IP on a single
NIC.

## Milestone 11: SMP (current)

Goal: use every CPU core QEMU gives us, not just the one UEFI hands
off on. Discovers every core via ACPI, boots each one into 64-bit long
mode through the classic INIT-SIPI-SIPI sequence and a real-mode
trampoline, and proves they're genuinely running in parallel with the
BSP -- distributing actual scheduled threads across them is future
work; this milestone is about getting other cores alive and executing
kernel code safely at all.

- **`kernel/src/acpi.c`**: parses just enough ACPI to find the MADT
  (Multiple APIC Description Table) and enumerate every enabled Local
  APIC entry -- no AML interpreter, no other tables. The RSDP address
  comes from `boot_info_t` (`boot/src/main.c` now searches UEFI's own
  Configuration Table for the ACPI 2.0/1.0 GUID) rather than scanning
  legacy BIOS memory regions -- the first thing tried was scanning the
  EBDA and BIOS ROM area the way a non-UEFI OS would, which found
  nothing under OVMF, before switching to the reliable UEFI-native
  path.
- **`kernel/src/lapic.c`**: Local APIC access (MMIO base from
  `IA32_APIC_BASE`, enabling it, sending INIT/SIPI interprocessor
  interrupts) -- used only to wake other cores; every actual hardware
  IRQ (timer, keyboard) still routes through the legacy 8259 PIC
  exactly as before.
- **`kernel/src/ap_trampoline.S`**: a from-scratch real-mode bootstrap
  -- the first 16-bit code this kernel has ever needed. Placed at a
  fixed physical address (`kernel/linker.ld` gives it its own PT_LOAD
  segment at `0x8000`, so the bootloader's existing generic
  per-segment loader places the bytes there automatically, no new
  build step). Walks 16-bit real mode -> 32-bit protected mode -> 64-bit
  long mode using a tiny temporary GDT, loads a real per-AP stack, and
  calls into `ap_entry()` (`smp.c`), which switches to the kernel's
  real GDT/IDT before doing anything else. Every address needed while
  still in 16-bit mode is computed as a compile-time constant
  (`TRAMPOLINE_BASE + label - ap_trampoline_start`) rather than a bare
  linker-resolved absolute reference -- 16-bit absolute relocations
  turned out to be an unreliable corner of an elf64-x86-64 toolchain.
- **`kernel/src/spinlock.c`**: the first real cross-core lock in this
  kernel (test-and-test-and-set via `xchg`). Every "critical section"
  before this milestone was a bare `cli`/`sti` pair, correct on one
  core (disabling interrupts stops the only other thing that could
  run: a preempting timer tick on that same core) but meaningless
  across cores, since `cli` on one core does nothing to stop another
  core running the same code at the same time. Wired in first for
  `kprintf`'s shared UART -- two cores printing concurrently otherwise
  interleave their bytes on the wire.
- **The actual bug, once everything above worked**: every AP
  triple-faulted the instant it touched its own (heap-allocated) stack,
  even though the exact same memory was already in daily use by the
  BSP. EFER is a *per-core* register -- `vmm_init()` sets EFER.NXE on
  the BSP only, and the identity map marks everything above
  `KERNEL_EXEC_LIMIT` (all of the heap included) NX. An AP starting
  fresh via SIPI has its own EFER with NXE=0, which turns that same NX
  bit into a *reserved-bit* violation from its point of view -- and
  since `ap_entry()` hadn't loaded a real IDT yet at the point of first
  stack use, that fault had nowhere survivable to go and took down the
  whole VM. Found by bisecting the trampoline instruction by instruction
  with raw port-I/O breadcrumbs (bypassing `kprintf` entirely) until the
  exact faulting operation was isolated, then fixed by setting EFER.NXE
  on the AP alongside EFER.LME.
- **A new self-test** (`smp_test_thread` in `kmain.c`) waits until
  every core smp.c started has incremented its own private counter
  past a threshold -- proof cores are actually executing in parallel,
  not just that `smp_init()` believed it started them. QEMU scripts
  gained `-smp 4`.

Not yet: distributing real scheduled threads/processes across cores
(every AP just spins incrementing its own counter forever), per-core
interrupt handling (only the BSP takes IRQs/exceptions/syscalls),
real spinlock coverage for the heap/page allocator/AHCI/e1000 (none of
those are touched concurrently yet, since APs don't run real work),
and CPU hotplug.

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
userland/ RebornOS's own user programs (ELF64, no libc, loaded off disk at boot)
tools/    ESP staging, QEMU run/test/debug scripts, panic symbolizer
toolchain/ builds the x86_64-elf-gcc cross-compiler from source
```

## Philosophy

The kernel provides sharp primitives; userspace builds the actual
world. Roadmap so far: physical memory allocator -> virtual memory /
heap -> interrupts & timers -> scheduler -> user mode & syscalls ->
per-process address space isolation -> VFS + a FAT16 filesystem + an
ELF loader for real disk-loaded programs -> a keyboard driver, real
thread exit, and an interactive shell -> PCI enumeration and a real
AHCI disk driver -> subdirectories, real argv, and disk write support
-> real virtual memory: a genuinely ring-0-only kernel mapping, real
per-process pointer validation, and page-fault-driven stack growth ->
networking: a polled e1000 driver plus Ethernet/ARP/ICMP, proven with a
real ping round-trip through QEMU's NAT -> SMP: every core discovered
via ACPI, booted through a real-mode trampoline into long mode, proven
running in parallel (done). Next: distributing real scheduled threads
across cores (today every AP just spins alone) is the leading
candidate for the next big push; UDP/TCP and a minimal HTTP responder,
piping (needs a real file-descriptor abstraction first), command
history, and shell quoting remain smaller open items. GUI, package
managers, and Linux binary compatibility are deliberately out of scope
until there's a command-line system that boots reliably, manages
memory correctly, runs isolated programs, and touches files.
