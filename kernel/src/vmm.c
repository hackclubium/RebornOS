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

static pte_t *alloc_table(void) {
    void *page = pmm_alloc_page();
    if (page == 0) {
        panic("vmm_init: out of physical memory while building page tables");
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
    pml4[0] = (uint64_t)(uintptr_t)pdpt | PAGE_PRESENT | PAGE_WRITABLE;

    for (int gib = 0; gib < IDENTITY_MAP_GIB; gib++) {
        pte_t *pd = alloc_table();
        pdpt[gib] = (uint64_t)(uintptr_t)pd | PAGE_PRESENT | PAGE_WRITABLE;

        for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
            uint64_t phys = (uint64_t)gib * GIB + (uint64_t)i * HUGE_PAGE_SIZE;
            pte_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
            if (phys >= KERNEL_EXEC_LIMIT) {
                flags |= PAGE_NX;
            }
            pd[i] = phys | flags;
        }
    }

    load_cr3((uint64_t)(uintptr_t)pml4);

    kprintf("vmm: own page tables live, identity-mapped 0-%uGiB (first %luMiB executable)\n",
            IDENTITY_MAP_GIB, KERNEL_EXEC_LIMIT / (1024 * 1024));
}
