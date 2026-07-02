/*
 * Kernel entry point. UEFI has already put the CPU in 64-bit long mode
 * with paging on, so there is no mode-transition assembly to write here
 * -- the only reason this is assembly rather than C is that the kernel
 * needs to switch off whatever stack the firmware happened to leave us
 * on and onto memory it owns outright before calling into C.
 *
 * The bootloader calls this symbol through a function pointer marked
 * __attribute__((sysv_abi)), so the boot_info_t* argument arrives in
 * %rdi per the standard System V AMD64 calling convention.
 */

.section .text
.global _start
.extern kmain

_start:
    lea stack_top(%rip), %rsp
    call kmain

.hang:
    cli
    hlt
    jmp .hang

.section .bss
.align 16
stack_bottom:
    .skip 65536 /* 64 KiB kernel boot stack */
stack_top:
