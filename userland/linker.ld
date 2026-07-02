ENTRY(_start)

/* Must match ELF_USER_LOAD_BASE in kernel/include/elf_loader.h -- that's
 * the address the kernel's ELF loader assumes every userland program
 * was linked at when it maps PT_LOAD segments into a process's private
 * PML4[1] range. */
USER_LOAD_ADDR = 0x8000400000;

SECTIONS
{
    . = USER_LOAD_ADDR;

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
