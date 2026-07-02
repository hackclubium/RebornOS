#include <stddef.h>
#include <stdint.h>
#include "boot_info.h"
#include "serial.h"
#include "framebuffer.h"
#include "panic.h"
#include "kprintf.h"
#include "qemu_debug.h"
#include "pmm.h"

void kmain(boot_info_t *info) {
    serial_init();
    kprintf("RebornOS kernel: serial online\n");

    if (info == NULL || info->magic != BOOT_INFO_MAGIC) {
        panic("invalid boot_info handoff (magic mismatch)");
    }

    kprintf("boot_info OK: framebuffer %ux%u @ 0x%lx (%lu bytes)\n",
            info->framebuffer.width, info->framebuffer.height,
            info->framebuffer.base, info->framebuffer.size);
    kprintf("memory map: %lu bytes, descriptor size %lu, version %u\n",
            info->memory_map_size, info->memory_map_descriptor_size,
            info->memory_map_descriptor_version);

    fb_init(&info->framebuffer);
    fb_clear(0x00102030);
    fb_puts(8, 8, "REBORNOS -- MILESTONE 1: PHYSICAL MEMORY", 0xFFFFFFFF, 0x00102030);
    fb_puts(8, 24, "SERIAL + FRAMEBUFFER ALIVE.", 0xFFA0A0A0, 0x00102030);

    kprintf("kmain: boot checks complete\n");

    pmm_init(info);

#ifdef REBORNOS_TEST_MODE
    /* Exercise the allocator: distinct pages, independently writable,
     * freed pages become reusable. Any failure panics instead of
     * silently reporting success. */
    void *p1 = pmm_alloc_page();
    void *p2 = pmm_alloc_page();
    if (p1 == NULL || p2 == NULL || p1 == p2) {
        panic("pmm self-test: alloc returned null or a duplicate page");
    }

    *(volatile uint8_t *)p1 = 0xAB;
    *(volatile uint8_t *)p2 = 0xCD;
    if (*(volatile uint8_t *)p1 != 0xAB || *(volatile uint8_t *)p2 != 0xCD) {
        panic("pmm self-test: allocated pages are not independently writable");
    }

    uint64_t free_before = pmm_free_page_count();
    pmm_free_page(p1);
    pmm_free_page(p2);
    if (pmm_free_page_count() != free_before + 2) {
        panic("pmm self-test: free count didn't increase after freeing pages");
    }

    void *p3 = pmm_alloc_page();
    if (p3 == NULL) {
        panic("pmm self-test: re-alloc after free failed");
    }

    kprintf("TEST MODE: pmm self-test passed\n");
    qemu_debug_exit(QEMU_DEBUG_EXIT_SUCCESS);
#endif

    {
        uint64_t total_mib = (pmm_total_pages() * PMM_PAGE_SIZE) / (1024 * 1024);
        uint64_t free_mib = (pmm_free_page_count() * PMM_PAGE_SIZE) / (1024 * 1024);
        kprintf("pmm ready: %lu MiB total, %lu MiB free\n", total_mib, free_mib);
    }

    kprintf("kmain: halting (no virtual memory or scheduler yet -- later milestones)\n");
    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}
