/*
 * Real-mode bootstrap for an Application Processor (a CPU core other
 * than the one we already booted on -- the BSP). After smp.c sends it
 * a Startup IPI, an AP begins executing here in 16-bit real mode at
 * CS:IP = (vector<<8):0000 -- vector is AP_TRAMPOLINE_ADDR>>12, so the
 * linear starting address is exactly AP_TRAMPOLINE_ADDR (see
 * kernel/linker.ld, which places this whole file's .ap_trampoline
 * section there as its own PT_LOAD segment, so the bytes are already
 * physically resident there from the moment the kernel starts -- no
 * runtime copy needed).
 *
 * Every *32/64-bit* label reference below resolves to its real
 * physical address via the ordinary linker, same as anywhere else in
 * this kernel. The handful of addresses needed *while still in 16-bit
 * mode* (the LGDT operand, the far jump into 32-bit code) are instead
 * computed as TRAMPOLINE_BASE + (label - ap_trampoline_start): plain
 * compile-time constants (a label difference within one section never
 * needs a relocation, regardless of target format), not a bare
 * absolute symbol reference -- 16-bit absolute relocations are a
 * genuinely obscure, unreliably-supported corner of an elf64-x86-64
 * toolchain, and this sidesteps needing one entirely. TRAMPOLINE_BASE
 * must match AP_TRAMPOLINE_ADDR in kernel/linker.ld.
 *
 * Walks real mode -> 32-bit protected mode -> 64-bit long mode using a
 * tiny temporary flat GDT (this file's own, not the kernel's shared
 * one), then loads the real per-AP stack and jumps into ap_entry() (a
 * plain C function, see smp.c) with the CPU otherwise unconfigured --
 * ap_entry() itself loads the kernel's real GDT/IDT before doing
 * anything else.
 *
 * smp.c writes ap_cr3/ap_stack_top before triggering each AP's SIPI
 * (bring-up is strictly one-AP-at-a-time, so these three slots are
 * safely reused for every AP in turn).
 */

.equ TRAMPOLINE_BASE, 0x8000

.section .ap_trampoline, "awx"

.code16
.global ap_trampoline_start
ap_trampoline_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7c00, %sp /* conventional, well-known-free scratch stack --
                       * only needed for the handful of instructions
                       * before the real per-AP stack is loaded below */

    lgdt (TRAMPOLINE_BASE + gdt32_descriptor - ap_trampoline_start)

    mov %cr0, %eax
    or $1, %eax /* CR0.PE */
    mov %eax, %cr0

    ljmp $0x08, $(TRAMPOLINE_BASE + pm32_entry - ap_trampoline_start)

.code32
pm32_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    mov $0x7c00, %esp

    mov %cr4, %eax
    or $(1 << 5), %eax /* CR4.PAE */
    mov %eax, %cr4

    mov ap_cr3, %eax /* low 32 bits are enough -- every physical address this kernel hands out fits well within 4 GiB */
    mov %eax, %cr3

    /* EFER is a per-core register -- vmm_init() (kernel/src/vmm.c) set
     * NXE on the BSP only, when it built the identity map that marks
     * everything above KERNEL_EXEC_LIMIT (all of the heap, this AP's
     * own stack included) as NX. An AP starting fresh via SIPI has its
     * own EFER with NXE=0, which turns that NX bit into a *reserved*
     * bit as far as this core is concerned -- the first access to any
     * such page (e.g. this AP's own stack, the moment it's used)
     * faults on a reserved-bit violation, and since ap_entry() hasn't
     * loaded a real IDT yet at this point, that fault has nowhere
     * survivable to go and triple-faults the whole machine. Setting
     * NXE here, alongside LME, is what actually fixed that. */
    mov $0xC0000080, %ecx /* IA32_EFER */
    rdmsr
    or $((1 << 8) | (1 << 11)), %eax /* EFER.LME | EFER.NXE */
    wrmsr

    mov %cr0, %eax
    or $(1 << 31), %eax /* CR0.PG -- activates long mode (compatibility submode, since CS is still 32-bit) */
    mov %eax, %cr0

    ljmp $0x18, $lm64_entry

.code64
lm64_entry:
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    mov ap_stack_top(%rip), %rsp
    mov ap_entry_point(%rip), %rax
    call *%rax

.hang:
    cli
    hlt
    jmp .hang

.align 16
gdt32_start:
    .quad 0x0000000000000000 /* null */
    .quad 0x00CF9A000000FFFF /* 0x08: 32-bit flat code, base=0 limit=4GiB G=1 D=1 */
    .quad 0x00CF92000000FFFF /* 0x10: 32-bit flat data */
    .quad 0x00AF9A000000FFFF /* 0x18: 64-bit code, L=1 D=0 (matches gdt.c's own encoding) */
    .quad 0x00CF92000000FFFF /* 0x20: 64-bit data (base/limit are formalities in long mode) */
gdt32_end:

gdt32_descriptor:
    .word gdt32_end - gdt32_start - 1
    .long TRAMPOLINE_BASE + gdt32_start - ap_trampoline_start

/* Mailbox smp.c fills in before each AP's SIPI -- see smp.c. */
.global ap_cr3
ap_cr3:         .quad 0
.global ap_stack_top
ap_stack_top:   .quad 0
.global ap_entry_point
ap_entry_point: .quad 0

.global ap_trampoline_end
ap_trampoline_end:
