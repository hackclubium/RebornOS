#include <stdint.h>
#include "vmm.h"
#include "pmm.h"
#include "panic.h"
#include "kprintf.h"
#include "minilib.h"

#define ENTRIES_PER_TABLE 512
#define HUGE_PAGE_SIZE (2ULL * 1024 * 1024)
#define GIB (1ULL << 30)
#define IDENTITY_MAP_GIB 4       /* covers 0..4 GiB */
#define KERNEL_EXEC_LIMIT (4ULL * 1024 * 1024) /* first 4 MiB stays executable */

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)  /* accessible from ring 3 -- must be set at EVERY level of the
                                    * walk (PML4/PDPT/PD), not just the leaf, or the CPU treats the
                                    * page as supervisor-only regardless of the leaf's own bit */
#define PAGE_HUGE     (1ULL << 7)  /* PS bit: valid at the PD level, makes a 2 MiB leaf */
#define PAGE_NX       (1ULL << 63)

#define IA32_EFER     0xC0000080u
#define EFER_NXE      (1ULL << 11)

typedef uint64_t pte_t;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline void load_cr3(uint64_t phys_addr) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys_addr) : "memory");
}

static pte_t *kernel_pml4;

static pte_t *alloc_table(void) {
    void *page = pmm_alloc_page();
    if (page == 0) {
        panic("vmm: out of physical memory while building page tables");
    }
    memset(page, 0, PMM_PAGE_SIZE);
    return (pte_t *)page;
}

void vmm_init(void) {
    /* NX-marked entries are treated as reserved-bit violations (an
     * instant #GP/#PF) unless EFER.NXE is set first. */
    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | EFER_NXE);

    pte_t *pml4 = alloc_table();
    pte_t *pdpt = alloc_table();
    pml4[0] = (uint64_t)(uintptr_t)pdpt | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    for (int gib = 0; gib < IDENTITY_MAP_GIB; gib++) {
        pte_t *pd = alloc_table();
        pdpt[gib] = (uint64_t)(uintptr_t)pd | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

        for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
            uint64_t phys = (uint64_t)gib * GIB + (uint64_t)i * HUGE_PAGE_SIZE;
            /* PAGE_USER on the whole identity map: this low-4GiB range
             * is the "system" mapping every address space shares
             * verbatim via PML4[0] (see vmm_create_address_space) --
             * kernel code, the heap, the framebuffer, all identical
             * for every process. Per-process isolation lives entirely
             * in PML4[1], a separate private mapping each address
             * space gets its own copy of. */
            pte_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_HUGE;
            if (phys >= KERNEL_EXEC_LIMIT) {
                flags |= PAGE_NX;
            }
            pd[i] = phys | flags;
        }
    }

    kernel_pml4 = pml4;
    load_cr3((uint64_t)(uintptr_t)pml4);

    kprintf("vmm: own page tables live, identity-mapped 0-%uGiB (first %luMiB executable)\n",
            IDENTITY_MAP_GIB, KERNEL_EXEC_LIMIT / (1024 * 1024));
}

uint64_t vmm_kernel_cr3(void) {
    return (uint64_t)(uintptr_t)kernel_pml4;
}

void vmm_load_cr3(uint64_t phys_addr) {
    load_cr3(phys_addr);
}

uint64_t vmm_create_address_space(void) {
    pte_t *pml4 = alloc_table();
    pml4[0] = kernel_pml4[0]; /* share the low-4GiB system mapping verbatim */

    pte_t *pdpt = alloc_table();
    pte_t *pd = alloc_table();
    pte_t *pt = alloc_table();
    void *private_page = pmm_alloc_page();
    if (private_page == 0) {
        panic("vmm_create_address_space: out of physical memory for the private page");
    }
    memset(private_page, 0, PMM_PAGE_SIZE);

    pte_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    pt[0] = (uint64_t)(uintptr_t)private_page | flags;
    pd[0] = (uint64_t)(uintptr_t)pt | flags;
    pdpt[0] = (uint64_t)(uintptr_t)pd | flags;
    pml4[1] = (uint64_t)(uintptr_t)pdpt | flags;

    return (uint64_t)(uintptr_t)pml4;
}

#define TABLE_ADDR_MASK (~0xFFFULL) /* strips flag bits, leaving just the 4KiB-aligned physical address */

void vmm_map_page(uint64_t pml4_phys, uint64_t vaddr, uint64_t paddr, uint32_t flags) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;

    if (pml4_idx == 0) {
        panic("vmm_map_page: refusing to map vaddr 0x%lx into PML4[0] (the shared kernel mapping)", vaddr);
    }

    pte_t *pml4 = (pte_t *)(uintptr_t)pml4_phys;
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        pml4[pml4_idx] = (uint64_t)(uintptr_t)alloc_table() | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }
    pte_t *pdpt = (pte_t *)(uintptr_t)(pml4[pml4_idx] & TABLE_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        pdpt[pdpt_idx] = (uint64_t)(uintptr_t)alloc_table() | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }
    pte_t *pd = (pte_t *)(uintptr_t)(pdpt[pdpt_idx] & TABLE_ADDR_MASK);

    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        pd[pd_idx] = (uint64_t)(uintptr_t)alloc_table() | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }
    pte_t *pt = (pte_t *)(uintptr_t)(pd[pd_idx] & TABLE_ADDR_MASK);

    pte_t leaf_flags = PAGE_PRESENT | PAGE_USER;
    if (flags & VMM_MAP_WRITE) {
        leaf_flags |= PAGE_WRITABLE;
    }
    if (!(flags & VMM_MAP_EXEC)) {
        leaf_flags |= PAGE_NX;
    }
    pt[pt_idx] = (paddr & TABLE_ADDR_MASK) | leaf_flags;
}
