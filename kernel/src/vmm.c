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

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

#define TABLE_ADDR_MASK (~0xFFFULL) /* strips flag bits, leaving just the 4KiB-aligned physical address */

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
    /* PAGE_USER stays set at these two intermediate levels -- x86
     * page-walk permissions are the AND of the User bit across every
     * level, so if this level ever cleared it, the per-leaf distinction
     * below could never take effect no matter what the leaf says.
     * Whether ring 3 can actually reach any given page is decided
     * entirely by that page's own leaf entry. */
    pml4[0] = (uint64_t)(uintptr_t)pdpt | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    for (int gib = 0; gib < IDENTITY_MAP_GIB; gib++) {
        pte_t *pd = alloc_table();
        pdpt[gib] = (uint64_t)(uintptr_t)pd | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

        for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
            uint64_t phys = (uint64_t)gib * GIB + (uint64_t)i * HUGE_PAGE_SIZE;
            /* Only the executable first KERNEL_EXEC_LIMIT bytes keep
             * PAGE_USER: ring3_program/process_body (kmain.c) are
             * ordinary kernel functions invoked directly via
             * enter_usermode() as a lightweight ring3-transition test
             * that predates the ELF loader, and their code lives in
             * this same low kernel-image region, so it has to stay
             * executable from ring 3. Every real user program instead
             * gets its own private PML4[1] window (see
             * vmm_create_address_space()) -- this narrow legacy slice
             * is the only other place a leaf in this identity map ever
             * carries PAGE_USER, so everything else here (the heap, the
             * page tables themselves, the framebuffer, ...) stays
             * genuinely off-limits to ring 3, which is what
             * vmm_check_range() relies on when validating a syscall's
             * user-supplied pointers. */
            pte_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
            if (phys < KERNEL_EXEC_LIMIT) {
                flags |= PAGE_USER;
            } else {
                flags |= PAGE_NX;
            }
            pd[i] = phys | flags;
        }
    }

    kernel_pml4 = pml4;
    load_cr3((uint64_t)(uintptr_t)pml4);

    kprintf("vmm: own page tables live, identity-mapped 0-%uGiB (first %luMiB executable, ring0-only)\n",
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

uint64_t vmm_current_cr3(void) {
    return read_cr3();
}

/* Walks pml4_phys down to vaddr's leaf entry without allocating
 * anything missing (unlike vmm_map_page()) -- a leaf is either a 4KiB
 * PT entry, or a 2MiB PD entry if PAGE_HUGE is set (the kernel identity
 * map in vmm_init() uses huge pages; per-process PML4[1] mappings from
 * vmm_map_page() never do, but a walker needs to handle both to be
 * correct for any pml4_phys it's given). Returns NULL if any level
 * isn't present. */
static pte_t *walk_to_leaf(uint64_t pml4_phys, uint64_t vaddr) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;

    pte_t *pml4 = (pte_t *)(uintptr_t)pml4_phys;
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        return NULL;
    }
    pte_t *pdpt = (pte_t *)(uintptr_t)(pml4[pml4_idx] & TABLE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        return NULL;
    }
    pte_t *pd = (pte_t *)(uintptr_t)(pdpt[pdpt_idx] & TABLE_ADDR_MASK);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        return NULL;
    }
    if (pd[pd_idx] & PAGE_HUGE) {
        return &pd[pd_idx];
    }
    pte_t *pt = (pte_t *)(uintptr_t)(pd[pd_idx] & TABLE_ADDR_MASK);
    if (!(pt[pt_idx] & PAGE_PRESENT)) {
        return NULL;
    }
    return &pt[pt_idx];
}

int vmm_page_present(uint64_t pml4_phys, uint64_t vaddr) {
    return walk_to_leaf(pml4_phys, vaddr) != NULL;
}

int vmm_check_range(uint64_t pml4_phys, uint64_t vaddr, uint64_t len, int need_write) {
    if (len == 0) {
        return 1;
    }
    if (vaddr + len < vaddr) {
        return 0; /* overflow */
    }

    uint64_t start = vaddr & ~(uint64_t)(PMM_PAGE_SIZE - 1);
    uint64_t end = vaddr + len; /* exclusive */
    for (uint64_t page = start; page < end; page += PMM_PAGE_SIZE) {
        pte_t *leaf = walk_to_leaf(pml4_phys, page);
        if (leaf == NULL || !(*leaf & PAGE_USER)) {
            return 0;
        }
        if (need_write && !(*leaf & PAGE_WRITABLE)) {
            return 0;
        }
    }
    return 1;
}
