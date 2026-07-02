#ifndef REBORNOS_ELF_LOADER_H
#define REBORNOS_ELF_LOADER_H

#include <stdint.h>

/* Where our own userland programs are linked to run. Must fall inside
 * PML4[1]'s 512 GiB range (see vmm.h's PROCESS_PRIVATE_VADDR) -- never
 * inside the shared low-4GiB identity map every process's PML4[0]
 * points at verbatim. Offset by 4 MiB from PROCESS_PRIVATE_VADDR itself
 * so a loaded program's segments never collide with the fixed demo
 * page vmm_create_address_space() always carves out at that exact
 * address. Every userland linker script must hardcode this same value
 * as its load address. */
#define ELF_USER_LOAD_BASE 0x0000008000400000ULL

/* The user stack lives far above the load base (256 MiB in), leaving
 * plenty of room to grow the program image before the two could ever
 * meet -- more than enough for the tiny static binaries this milestone
 * loads. Grows down from here. */
#define ELF_USER_STACK_TOP 0x0000008010000000ULL
#define ELF_USER_STACK_PAGES 4u /* 16 KiB */

typedef struct {
    uint64_t entry;
    uint64_t stack_top;
} elf_process_t;

/* Parses elf_data (elf_size bytes, already read fully into a kernel
 * buffer -- see vfs.h) as a static ELF64 executable, maps its PT_LOAD
 * segments plus a fresh stack into the address space rooted at
 * pml4_phys (see vmm_create_address_space()), and fills *out with the
 * entry point and stack top ready to hand to enter_usermode(). Panics
 * on a malformed ELF -- our own toolchain produced this file, so a bad
 * header means something is fundamentally broken, not a case to
 * recover from gracefully. */
void elf_load_user_program(const uint8_t *elf_data, uint64_t elf_size, uint64_t pml4_phys, elf_process_t *out);

#endif /* REBORNOS_ELF_LOADER_H */
