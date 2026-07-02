#ifndef REBORNOS_VMM_H
#define REBORNOS_VMM_H

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

#endif /* REBORNOS_VMM_H */
