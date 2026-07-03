#include <stdint.h>
#include "elf_loader.h"
#include "elf64.h"
#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include "minilib.h"
#include "gdt.h"
#include "heap.h"

static uint64_t page_align_down(uint64_t x) {
    return x & ~(uint64_t)(PMM_PAGE_SIZE - 1);
}

static uint64_t page_align_up(uint64_t x) {
    return (x + PMM_PAGE_SIZE - 1) & ~(uint64_t)(PMM_PAGE_SIZE - 1);
}

/* Packs argc strings plus a NULL-terminated pointer array into the top
 * of the topmost stack page (whose physical/virtual address the caller
 * already knows, having just allocated and mapped it) and returns the
 * new stack top -- pushed down below the blob so the process's own
 * stack usage grows away from it instead of into it. Returns
 * ELF_USER_STACK_TOP unchanged and *out_argv_ptr = 0 when argc is 0. */
static uint64_t write_argv_blob(void *top_page_phys, uint64_t top_page_vaddr,
                                 int argc, char **argv, uint64_t *out_argv_ptr) {
    if (argc <= 0) {
        *out_argv_ptr = 0;
        return ELF_USER_STACK_TOP;
    }
    if ((uint32_t)argc > ELF_ARGV_MAX_ARGS) {
        panic("elf_load_user_program: too many arguments (%d, max %u)", argc, ELF_ARGV_MAX_ARGS);
    }

    uint64_t lengths[ELF_ARGV_MAX_ARGS];
    uint64_t total_string_bytes = 0;
    for (int i = 0; i < argc; i++) {
        lengths[i] = strlen(argv[i]) + 1;
        total_string_bytes += lengths[i];
    }

    uint64_t string_area_bytes = (total_string_bytes + 7) & ~7ULL; /* keep the pointer array 8-byte aligned */
    uint64_t pointer_array_bytes = (uint64_t)(argc + 1) * sizeof(char *);
    uint64_t blob_bytes = string_area_bytes + pointer_array_bytes;
    if (blob_bytes > ELF_ARGV_MAX_TOTAL_BYTES) {
        panic("elf_load_user_program: argv too large (%lu bytes, max %u)",
              (uint64_t)blob_bytes, ELF_ARGV_MAX_TOTAL_BYTES);
    }
    blob_bytes = (blob_bytes + 15) & ~15ULL; /* keep the resulting stack top 16-byte aligned */

    uint64_t blob_vaddr = ELF_USER_STACK_TOP - blob_bytes;
    uint8_t *blob_phys = (uint8_t *)top_page_phys + (blob_vaddr - top_page_vaddr);

    uint8_t *string_dst = blob_phys;
    uint64_t string_vaddr = blob_vaddr;
    uint64_t *ptr_array = (uint64_t *)(blob_phys + string_area_bytes);
    uint64_t ptr_array_vaddr = blob_vaddr + string_area_bytes;

    for (int i = 0; i < argc; i++) {
        memcpy(string_dst, argv[i], lengths[i]);
        ptr_array[i] = string_vaddr;
        string_dst += lengths[i];
        string_vaddr += lengths[i];
    }
    ptr_array[argc] = 0; /* argv[argc] == NULL, matching the usual C convention */

    *out_argv_ptr = ptr_array_vaddr;
    return blob_vaddr;
}

void elf_load_user_program(const uint8_t *elf_data, uint64_t elf_size, uint64_t pml4_phys,
                            int argc, char **argv, elf_process_t *out) {
    if (elf_size < sizeof(Elf64_Ehdr)) {
        panic("elf_load_user_program: file too small to hold an ELF header");
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;
    if (ehdr->e_ident[0] != ELF_MAGIC0 || ehdr->e_ident[1] != ELF_MAGIC1 ||
        ehdr->e_ident[2] != ELF_MAGIC2 || ehdr->e_ident[3] != ELF_MAGIC3) {
        panic("elf_load_user_program: bad ELF magic");
    }
    if (ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_machine != EM_X86_64) {
        panic("elf_load_user_program: not a 64-bit x86_64 ELF");
    }

    const Elf64_Phdr *phdrs = (const Elf64_Phdr *)(elf_data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > elf_size) {
            panic("elf_load_user_program: PT_LOAD segment %u runs past end of file", (unsigned)i);
        }

        uint64_t seg_start = page_align_down(ph->p_vaddr);
        uint64_t seg_end = page_align_up(ph->p_vaddr + ph->p_memsz);

        uint32_t vmm_flags = 0;
        if (ph->p_flags & PF_W) {
            vmm_flags |= VMM_MAP_WRITE;
        }
        if (ph->p_flags & PF_X) {
            vmm_flags |= VMM_MAP_EXEC;
        }

        for (uint64_t page_vaddr = seg_start; page_vaddr < seg_end; page_vaddr += PMM_PAGE_SIZE) {
            void *phys = pmm_alloc_page();
            if (phys == 0) {
                panic("elf_load_user_program: out of physical memory loading segment %u", (unsigned)i);
            }
            memset(phys, 0, PMM_PAGE_SIZE);

            /* Copy whatever part of this page overlaps the segment's
             * file-backed bytes; anything beyond p_filesz (bss) stays
             * zeroed from the memset above. */
            uint64_t page_end = page_vaddr + PMM_PAGE_SIZE;
            uint64_t file_start = ph->p_vaddr;
            uint64_t file_end = ph->p_vaddr + ph->p_filesz;
            uint64_t copy_start = page_vaddr > file_start ? page_vaddr : file_start;
            uint64_t copy_end = page_end < file_end ? page_end : file_end;
            if (copy_end > copy_start) {
                uint64_t file_offset = ph->p_offset + (copy_start - ph->p_vaddr);
                uint64_t dst_offset = copy_start - page_vaddr;
                memcpy((uint8_t *)phys + dst_offset, elf_data + file_offset, copy_end - copy_start);
            }

            vmm_map_page(pml4_phys, page_vaddr, (uint64_t)(uintptr_t)phys, vmm_flags);
        }
    }

    void *top_page_phys = NULL;
    uint64_t top_page_vaddr = 0;

    for (uint32_t i = 0; i < ELF_USER_STACK_PAGES; i++) {
        void *phys = pmm_alloc_page();
        if (phys == 0) {
            panic("elf_load_user_program: out of physical memory for the user stack");
        }
        memset(phys, 0, PMM_PAGE_SIZE);
        uint64_t vaddr = ELF_USER_STACK_TOP - (uint64_t)(i + 1) * PMM_PAGE_SIZE;
        vmm_map_page(pml4_phys, vaddr, (uint64_t)(uintptr_t)phys, VMM_MAP_WRITE);
        if (i == 0) {
            top_page_phys = phys;
            top_page_vaddr = vaddr;
        }
    }

    uint64_t argv_ptr = 0;
    uint64_t stack_top = write_argv_blob(top_page_phys, top_page_vaddr, argc, argv, &argv_ptr);

    out->entry = ehdr->e_entry;
    out->stack_top = stack_top;
    out->argv_ptr = argv_ptr;
    out->argc = argc;
}

__attribute__((noreturn)) void elf_launch_process(void *arg) {
    elf_process_t *proc = (elf_process_t *)arg;
    uint64_t entry = proc->entry;
    uint64_t stack_top = proc->stack_top;
    int argc = proc->argc;
    uint64_t argv_ptr = proc->argv_ptr;
    kfree(proc);
    enter_usermode((void (*)(void))(uintptr_t)entry, (void *)(uintptr_t)stack_top,
                   GDT_USER_CODE_SEL, GDT_USER_DATA_SEL, argc, (char **)(uintptr_t)argv_ptr);
}
