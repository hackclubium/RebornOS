#ifndef REBORNOS_ELF64_H
#define REBORNOS_ELF64_H

/* Minimal ELF64 structures -- just enough to validate an ELF image and
 * walk its PT_LOAD program headers. This is the standard System V ABI
 * layout (stable since the 1990s), not UEFI-specific. Shared by the
 * bootloader (loading kernel.elf) and the kernel (loading user
 * programs off disk), since both are doing the same walk. */

#include <stdint.h>

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1

#define PF_X 0x1u /* executable */
#define PF_W 0x2u /* writable */
#define PF_R 0x4u /* readable (every segment we care about is readable in practice) */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#endif /* REBORNOS_ELF64_H */
