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

/* argc/argv, when requested, get packed into the top of the stack page
 * this loader already allocates -- a handful of short strings plus a
 * pointer array easily fits in one 4KiB page alongside the modest
 * amount of real stack usage these small programs ever need. See
 * elf_load_user_program()'s scope note. */
#define ELF_ARGV_MAX_ARGS 16u
#define ELF_ARGV_MAX_TOTAL_BYTES 3072u /* generous, but bounded well within one stack page */

typedef struct {
    uint64_t entry;
    uint64_t stack_top; /* below ELF_USER_STACK_TOP by the argv blob's size, if there is one */
    uint64_t argv_ptr;  /* virtual address (in the *child's* address space) of a NULL-terminated
                          * char* array -- 0 if argc was 0 */
    int argc;
} elf_process_t;

/* Parses elf_data (elf_size bytes, already read fully into a kernel
 * buffer -- see vfs.h) as a static ELF64 executable, maps its PT_LOAD
 * segments plus a fresh stack into the address space rooted at
 * pml4_phys (see vmm_create_address_space()), and fills *out with the
 * entry point and stack top ready to hand to enter_usermode(). Panics
 * on a malformed ELF -- our own toolchain produced this file, so a bad
 * header means something is fundamentally broken, not a case to
 * recover from gracefully.
 *
 * argv is a kernel-side array of argc NUL-terminated strings (already
 * copied out of whatever process asked for this one to be spawned --
 * see syscall.c's SYS_EXEC); pass argc=0, argv=NULL for a process that
 * takes no arguments. The strings get copied into the *new* process's
 * own stack memory before it ever runs, so the caller's copies don't
 * need to outlive this call. */
void elf_load_user_program(const uint8_t *elf_data, uint64_t elf_size, uint64_t pml4_phys,
                            int argc, char **argv, elf_process_t *out);

/* Thread entry point for a freshly spawned process (see process.h):
 * takes ownership of `arg` (an elf_process_t* from
 * elf_load_user_program(), heap-allocated by the caller -- freed here),
 * drops into ring 3 at its entry point, and never returns. */
__attribute__((noreturn)) void elf_launch_process(void *arg);

#endif /* REBORNOS_ELF_LOADER_H */
