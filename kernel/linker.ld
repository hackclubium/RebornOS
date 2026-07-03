ENTRY(_start)

/* 1 MiB: conventional freestanding load address, well within the
 * identity-mapped region UEFI's page tables cover at handoff time. */
KERNEL_LOAD_ADDR = 0x100000;

/* Where an Application Processor starts executing in real mode after
 * a Startup IPI (see smp.c/ap_trampoline.S) -- must be 4 KiB-aligned,
 * since the SIPI vector encodes it as a page number. Placing this
 * code in its own PT_LOAD segment at this fixed *physical* address
 * means the bootloader's existing generic PT_LOAD-loading loop
 * (boot/src/main.c already places every segment at its own p_paddr)
 * puts these bytes there automatically -- no bootloader changes, no
 * separate build/embed step, and every label inside ap_trampoline.S
 * resolves to its real runtime address via the ordinary linker, not
 * hand-computed offsets. */
AP_TRAMPOLINE_ADDR = 0x8000;

PHDRS
{
    trampoline PT_LOAD FLAGS(7); /* R+W+X */
    kernel PT_LOAD FLAGS(7);
}

SECTIONS
{
    . = AP_TRAMPOLINE_ADDR;
    .ap_trampoline : {
        *(.ap_trampoline)
    } :trampoline

    . = KERNEL_LOAD_ADDR;

    .text : {
        *(.text*)
    } :kernel

    .rodata : {
        *(.rodata*)
    } :kernel

    .data : {
        *(.data*)
    } :kernel

    .bss : {
        *(COMMON)
        *(.bss*)
    } :kernel

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
