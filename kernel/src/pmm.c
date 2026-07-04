#include <stddef.h>
#include "pmm.h"
#include "efi_memory_map.h"
#include "kprintf.h"
#include "panic.h"
#include "minilib.h"
#include "interrupts.h"
#include "spinlock.h"

static uint8_t *bitmap;
static uint64_t bitmap_bits;   /* one bit per page; bit N == physical address N * PMM_PAGE_SIZE */
static uint64_t free_pages;
static uint64_t search_hint;   /* bit index to resume the next alloc scan from */

static inline int type_is_usable(uint32_t type) {
    return type == EFI_MEM_TYPE_CONVENTIONAL ||
           type == EFI_MEM_TYPE_BOOT_SERVICES_CODE ||
           type == EFI_MEM_TYPE_BOOT_SERVICES_DATA;
}

static inline void bitmap_set(uint64_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static inline void bitmap_clear(uint64_t bit) {
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static inline int bitmap_test(uint64_t bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static efi_memory_descriptor_t *desc_at(const boot_info_t *info, uint64_t index) {
    uint8_t *base = (uint8_t *)(uintptr_t)info->memory_map_addr;
    return (efi_memory_descriptor_t *)(base + index * info->memory_map_descriptor_size);
}

void pmm_init(const boot_info_t *info) {
    uint64_t descriptor_count = info->memory_map_size / info->memory_map_descriptor_size;

    /* Pass 1: find the highest physical address any *usable* descriptor
     * covers, so the bitmap spans exactly the range we might actually
     * allocate from. Deliberately not "every descriptor": firmware
     * reports all sorts of things as EfiReservedMemoryType, including
     * small legitimate pockets inside real RAM but also, on this q35
     * setup, a ~1 TiB-wide 64-bit PCI MMIO window at a physical address
     * with nothing to do with installed RAM. Since every non-usable
     * type is already permanently marked "used" below regardless, a
     * region we'll never allocate from doesn't need bitmap coverage at
     * all -- there's no fragile "is this really RAM" type-guessing to
     * get right. */
    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < descriptor_count; i++) {
        efi_memory_descriptor_t *d = desc_at(info, i);
        if (!type_is_usable(d->type)) {
            continue;
        }
        uint64_t end = d->physical_start + d->number_of_pages * PMM_PAGE_SIZE;
        if (end > highest_addr) {
            highest_addr = end;
        }
    }
    bitmap_bits = highest_addr / PMM_PAGE_SIZE;
    uint64_t bitmap_bytes = (bitmap_bits + 7) / 8;

    /* Pass 2: first-fit a usable region big enough to host the bitmap
     * itself. This runs once at boot, so first-fit is fine. */
    efi_memory_descriptor_t *bitmap_region = NULL;
    for (uint64_t i = 0; i < descriptor_count; i++) {
        efi_memory_descriptor_t *d = desc_at(info, i);
        if (d->type != EFI_MEM_TYPE_CONVENTIONAL) {
            continue;
        }
        if (d->number_of_pages * PMM_PAGE_SIZE >= bitmap_bytes) {
            bitmap_region = d;
            break;
        }
    }
    if (!bitmap_region) {
        panic("pmm_init: no conventional memory region big enough for the page bitmap (%lu bytes)", bitmap_bytes);
    }
    bitmap = (uint8_t *)(uintptr_t)bitmap_region->physical_start;

    /* Default every page to used; only usable regions get cleared. */
    memset(bitmap, 0xFF, bitmap_bytes);
    free_pages = 0;

    for (uint64_t i = 0; i < descriptor_count; i++) {
        efi_memory_descriptor_t *d = desc_at(info, i);
        if (!type_is_usable(d->type)) {
            continue;
        }
        uint64_t start_page = d->physical_start / PMM_PAGE_SIZE;
        for (uint64_t p = 0; p < d->number_of_pages; p++) {
            bitmap_clear(start_page + p);
            free_pages++;
        }
    }

    /* The bitmap's own region is EfiConventionalMemory, so the loop
     * above just marked the pages it occupies as free -- claim them
     * back since they're actually spoken for. */
    uint64_t bitmap_pages = (bitmap_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t bitmap_start_page = bitmap_region->physical_start / PMM_PAGE_SIZE;
    for (uint64_t p = 0; p < bitmap_pages; p++) {
        if (!bitmap_test(bitmap_start_page + p)) {
            bitmap_set(bitmap_start_page + p);
            free_pages--;
        }
    }

    /* Physical page 0 is never handed out, full stop, regardless of
     * what firmware's memory map says about it. Every allocator here
     * (this one, and alloc_table() in vmm.c on top of it) uses a
     * NULL/0 return to mean "allocation failed" -- if address 0 were
     * ever actually given out, a *successful* allocation there would
     * be indistinguishable from failure. This used to work by pure
     * coincidence: whichever usable descriptor Pass 2 picked to host
     * the bitmap itself often happened to start at 0, incidentally
     * reserving it. A firmware/memory-map layout where address 0 sits
     * in its own separate descriptor (observed in CI, never locally)
     * breaks that coincidence, so vmm_init()'s very first page-table
     * allocation can land on physical 0 and get misread as failure --
     * exactly the "out of physical memory" panic this fixes. */
    if (bitmap_bits > 0 && !bitmap_test(0)) {
        bitmap_set(0);
        free_pages--;
    }

    search_hint = 1;

    kprintf("pmm: %lu total pages (%lu MiB), %lu free (%lu MiB), bitmap at 0x%lx (%lu bytes)\n",
            bitmap_bits, (bitmap_bits * PMM_PAGE_SIZE) / (1024 * 1024),
            free_pages, (free_pages * PMM_PAGE_SIZE) / (1024 * 1024),
            (uint64_t)(uintptr_t)bitmap, bitmap_bytes);
}

/* bitmap/free_pages/search_hint are global mutable state with no
 * per-thread copies, called from all over the preemptible kernel
 * (page table setup, AHCI init, process address spaces, ...) -- same
 * hazard as kmalloc/kfree in heap.c, same two-part fix: irq_save_disable()/
 * irq_restore() for same-core preemption, pmm_lock for a different core
 * touching the bitmap at the same time now that real scheduled threads
 * run on more than one core (see heap_lock's comment in heap.c for why
 * both are needed together, not just one or the other). */
static spinlock_t pmm_lock = SPINLOCK_INIT;

void *pmm_alloc_page(void) {
    uint64_t flags = irq_save_disable();
    spinlock_acquire(&pmm_lock);
    for (uint64_t i = 0; i < bitmap_bits; i++) {
        uint64_t bit = (search_hint + i) % bitmap_bits;
        if (!bitmap_test(bit)) {
            bitmap_set(bit);
            free_pages--;
            search_hint = bit + 1;
            spinlock_release(&pmm_lock);
            irq_restore(flags);
            return (void *)(uintptr_t)(bit * PMM_PAGE_SIZE);
        }
    }
    spinlock_release(&pmm_lock);
    irq_restore(flags);
    return NULL;
}

void pmm_free_page(void *phys_addr) {
    uint64_t flags = irq_save_disable();
    spinlock_acquire(&pmm_lock);
    uint64_t bit = (uint64_t)(uintptr_t)phys_addr / PMM_PAGE_SIZE;
    if (bit >= bitmap_bits) {
        panic("pmm_free_page: address 0x%lx is outside the tracked range", (uint64_t)(uintptr_t)phys_addr);
    }
    if (!bitmap_test(bit)) {
        panic("pmm_free_page: double free of page 0x%lx", (uint64_t)(uintptr_t)phys_addr);
    }
    bitmap_clear(bit);
    free_pages++;
    spinlock_release(&pmm_lock);
    irq_restore(flags);
}

void pmm_reserve_region(uint64_t phys_addr, uint64_t size) {
    uint64_t start_page = phys_addr / PMM_PAGE_SIZE;
    uint64_t end_page = (phys_addr + size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    for (uint64_t p = start_page; p < end_page && p < bitmap_bits; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            free_pages--;
        }
    }
}

uint64_t pmm_total_pages(void) {
    return bitmap_bits;
}

uint64_t pmm_free_page_count(void) {
    return free_pages;
}
