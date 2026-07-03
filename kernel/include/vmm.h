#ifndef REBORNOS_VMM_H
#define REBORNOS_VMM_H

#include <stdint.h>

/* Builds the kernel's own page tables, replacing UEFI's, and switches
 * CR3 to them. Identity-maps the low 4 GiB (physical == virtual for
 * every address this milestone touches, including the framebuffer)
 * with 2 MiB pages: the first 4 MiB is present+writable+executable to
 * cover the kernel image, everything else is present+writable+NX. Not
 * a precise W^X split (a 2 MiB page's permissions apply to everything
 * in it, and the kernel image is much smaller than 4 MiB) -- tightening
 * that to page-level granularity is a reasonable future refinement,
 * not required to say the kernel now owns its own address space.
 *
 * Only the executable first KERNEL_EXEC_LIMIT bytes are User-accessible
 * (a narrow carve-out for kmain.c's pre-ELF-loader ring3 test
 * functions, which run directly out of this region -- see vmm.c);
 * everything else -- the heap, the page tables themselves, the
 * framebuffer, all of it -- is ring0-only. Ring 0 can still reach any
 * of it regardless (the User bit only restricts ring-3 accesses), but
 * since a ring-3 program's own page tables share this exact mapping
 * verbatim via PML4[0] (see vmm_create_address_space()), this is what
 * makes kernel memory genuinely off-limits to user code rather than
 * just conventionally left alone. */
void vmm_init(void);

uint64_t vmm_kernel_cr3(void);
void vmm_load_cr3(uint64_t phys_addr);

/* Returns the CR3 currently loaded -- since a syscall or page fault
 * never switches address spaces (the CPU just changes privilege
 * level), this is always the faulting/calling process's own PML4,
 * cheaply available without any per-thread bookkeeping. */
uint64_t vmm_current_cr3(void);

/* Real per-process pointer validation: walks the page tables rooted at
 * pml4_phys and confirms every 4 KiB page covering [vaddr, vaddr+len)
 * is actually present and user-accessible (and, if need_write, also
 * writable) -- as opposed to a coarse "is this address in one of a
 * few known ranges" heuristic. Works for any buffer the process
 * legitimately has mapped (its own stack, its loaded segments, ...),
 * not just a handful of hardcoded regions. */
int vmm_check_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t len, int need_write);

/* True if `vaddr` has a present leaf mapping in pml4_phys's page
 * tables, without allocating anything missing -- lets a page-fault
 * handler distinguish "no page here yet, safe to demand-page in" from
 * "already mapped, this fault means something else" before calling
 * vmm_map_page() (which would otherwise silently clobber and leak an
 * existing mapping). */
int vmm_page_present(uint64_t pml4_phys, uint64_t vaddr);

/* Every process's page tables share PML4[0] verbatim (the low-4GiB
 * kernel/system mapping vmm_init() built -- identical for everyone,
 * since kernel code has to stay reachable across any privilege
 * transition regardless of which process is running). What actually
 * makes each address space distinct is PML4[1]: a freshly built,
 * process-private PDPT/PD/PT chain mapping exactly one page at this
 * fixed virtual address. Two processes using this same virtual address
 * end up backed by two different physical pages -- that's the whole
 * isolation demonstration for this milestone. A real process would
 * have much more than one private page; this is deliberately minimal. */
#define PROCESS_PRIVATE_VADDR 0x8000000000ULL

/* Returns the new address space's CR3 (physical PML4 address). */
uint64_t vmm_create_address_space(void);

/* Flags for vmm_map_page(), independent of the raw page-table bit
 * layout (vmm.c keeps that private). PRESENT and USER are implied --
 * every page this maps is a present, ring3-accessible page, since it
 * only exists to back a user program's segments. */
#define VMM_MAP_WRITE (1u << 0)
#define VMM_MAP_EXEC  (1u << 1)

/* Maps a single 4 KiB page at `vaddr` to physical page `paddr` inside
 * the address space rooted at `pml4_phys`, allocating any missing
 * PDPT/PD/PT tables along the way. Unlike vmm_create_address_space()'s
 * fixed single private page, this is the general-purpose primitive the
 * ELF loader uses to map a program's actual segments (arbitrary vaddr,
 * arbitrary count of pages, per-segment permissions).
 *
 * `pml4_phys` must be a PML4 built by vmm_create_address_space() (or
 * the kernel's own), and `vaddr` must fall inside PML4[1..511] -- i.e.
 * >= PROCESS_PRIVATE_VADDR's PML4 slot -- never inside PML4[0], which
 * is the single shared low-4GiB system mapping every process's page
 * tables point at verbatim; writing into it here would corrupt every
 * other process's kernel mapping instead of building a private one. */
void vmm_map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint32_t flags);

#endif /* REBORNOS_VMM_H */
