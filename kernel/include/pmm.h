#ifndef REBORNOS_PMM_H
#define REBORNOS_PMM_H

#include <stdint.h>
#include "boot_info.h"

#define PMM_PAGE_SIZE 4096u

/* Physical memory manager: a flat bitmap over the whole physical
 * address range the UEFI memory map covers, one bit per 4 KiB page.
 * Returns/accepts physical addresses. There is no virtual memory yet
 * (that's a later milestone) -- the kernel is still running under
 * UEFI's identity map, so physical and virtual addresses coincide for
 * now, and pages handed out here are directly dereferenceable. */
void pmm_init(const boot_info_t *info);
void *pmm_alloc_page(void);
void pmm_free_page(void *phys_addr);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_page_count(void);

#endif /* REBORNOS_PMM_H */
