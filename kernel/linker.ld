ENTRY(_start)

/* 1 MiB: conventional freestanding load address, well within the
 * identity-mapped region UEFI's page tables cover at handoff time. */
KERNEL_LOAD_ADDR = 0x100000;

SECTIONS
{
    . = KERNEL_LOAD_ADDR;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
