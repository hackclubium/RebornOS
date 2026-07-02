#ifndef REBORNOS_PMM_H
#define REBORNOS_PMM_H

#include <stdint.h>
#include "boot_info.h"

#define PMM_PAGE_SIZE 4096u

/* Physical memory manager: a flat bitmap over the whole physical
 * address range the UEFI memory map covers, one bit per 4 KiB page.
 * Returns/accepts physical addresses. The kernel's own page tables
 * (see vmm.h) identity-map the low 4 GiB, so physical and virtual
 * addresses still coincide for any address a page allocated here could
 * have. */
void pmm_init(const boot_info_t *info);
void *pmm_alloc_page(void);
void pmm_free_page(void *phys_addr);
uint64_t pmm_total_pages(void);
uint64_t pmm_free_page_count(void);

/* Marks [phys_addr, phys_addr + size) used without handing out a
 * specific page, e.g. to carve out a fixed region for the heap before
 * anything else can claim it. Safe to call on pages that are already
 * used (a byte-range, not necessarily page-aligned, so this rounds
 * outward to whole pages). */
void pmm_reserve_region(uint64_t phys_addr, uint64_t size);

#endif /* REBORNOS_PMM_H */
