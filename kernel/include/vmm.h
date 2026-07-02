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
 * not required to say the kernel now owns its own address space. */
void vmm_init(void);

uint64_t vmm_kernel_cr3(void);
void vmm_load_cr3(uint64_t phys_addr);

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

#endif /* REBORNOS_VMM_H */
