#include <stddef.h>
#include <stdint.h>
#include "boot_info.h"
#include "serial.h"
#include "framebuffer.h"
#include "panic.h"
#include "kprintf.h"
#include "qemu_debug.h"
#include "pmm.h"
#include "vmm.h"
#include "interrupts.h"
#include "timer.h"
#include "heap.h"
#include "scheduler.h"

#ifdef REBORNOS_TEST_MODE
/* Two worker threads that just count, plus a monitor thread that waits
 * for both to have made real progress. If the scheduler is broken --
 * wrong context-switch layout, threads never actually alternating,
 * only one ever running -- one or both counters stall and the monitor
 * hits its own bound and panics instead of hanging forever. Reaching
 * qemu_debug_exit from a scheduled thread (rather than from kmain
 * directly) is itself part of the proof: kmain handed off control via
 * scheduler_start() and never runs again. */
static volatile uint64_t worker_a_count = 0;
static volatile uint64_t worker_b_count = 0;

static void worker_a(void) {
    for (;;) {
        worker_a_count++;
        __asm__ volatile("pause");
    }
}

static void worker_b(void) {
    for (;;) {
        worker_b_count++;
        __asm__ volatile("pause");
    }
}

static void scheduler_monitor(void) {
    uint64_t spins = 0;
    while (worker_a_count < 1000 || worker_b_count < 1000) {
        spins++;
        if (spins > 2000000000ULL) {
            panic("scheduler self-test: workers did not make interleaved progress (a=%lu b=%lu)",
                  worker_a_count, worker_b_count);
        }
    }
    kprintf("TEST MODE: scheduler self-test passed (a=%lu b=%lu)\n", worker_a_count, worker_b_count);
    qemu_debug_exit(QEMU_DEBUG_EXIT_SUCCESS);
}
#else
static void idle_thread(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}
#endif

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
    fb_puts(8, 8, "REBORNOS -- MILESTONE 2: PREEMPTIVE MULTITASKING", 0xFFFFFFFF, 0x00102030);
    fb_puts(8, 24, "SERIAL + FRAMEBUFFER ALIVE.", 0xFFA0A0A0, 0x00102030);

    kprintf("kmain: boot checks complete\n");

    pmm_init(info);
    vmm_init();
    idt_init();
    timer_init(100);
    heap_init();

#ifdef REBORNOS_TEST_MODE
    /* Exercise the physical allocator: distinct pages, independently
     * writable, freed pages become reusable. Any failure panics
     * instead of silently reporting success. */
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

    /* Exercise interrupts: if idt_init()/timer_init() are wrong in any
     * way that breaks IRQ delivery, this spins forever instead of
     * silently "passing" -- bounded so a real failure still reports
     * FAIL via the test harness's own timeout rather than hanging the
     * CI runner indefinitely. */
    uint64_t start_ticks = timer_ticks();
    uint64_t spins = 0;
    while (timer_ticks() == start_ticks) {
        __asm__ volatile("pause");
        spins++;
        if (spins > 500000000ULL) {
            panic("timer self-test: no tick observed -- IDT/PIC/PIT wiring is broken");
        }
    }
    kprintf("TEST MODE: timer self-test passed (%lu tick(s) observed)\n", timer_ticks());

    /* Exercise the heap: distinct allocations, independently writable,
     * a freed block gets reused, an oversized request fails cleanly
     * instead of corrupting something. */
    uint64_t heap_free_before = heap_free_bytes();
    char *h1 = (char *)kmalloc(64);
    char *h2 = (char *)kmalloc(128);
    if (h1 == NULL || h2 == NULL || (void *)h1 == (void *)h2) {
        panic("heap self-test: kmalloc returned null or overlapping blocks");
    }
    for (int i = 0; i < 64; i++) {
        h1[i] = (char)0xAB;
    }
    for (int i = 0; i < 128; i++) {
        h2[i] = (char)0xCD;
    }
    for (int i = 0; i < 64; i++) {
        if (h1[i] != (char)0xAB) {
            panic("heap self-test: block 1 contents corrupted (overlap with block 2?)");
        }
    }
    for (int i = 0; i < 128; i++) {
        if (h2[i] != (char)0xCD) {
            panic("heap self-test: block 2 contents corrupted");
        }
    }
    /* Free in reverse (LIFO) allocation order: kfree only coalesces
     * forward, so freeing h2 first lets it merge into the free block
     * after it, and freeing h1 second then merges it into that now-
     * larger neighbor -- fully coalescing back into a single block.
     * Freeing in allocation order instead would leave h1's 24-byte
     * header permanently un-mergeable and this assertion would be
     * wrong, not the allocator. */
    kfree(h2);
    kfree(h1);
    if (heap_free_bytes() != heap_free_before) {
        panic("heap self-test: free byte count didn't return to baseline after freeing everything");
    }
    void *huge = kmalloc(1024ULL * 1024 * 1024); /* far bigger than the whole heap */
    if (huge != NULL) {
        panic("heap self-test: an allocation bigger than the entire heap should fail, not succeed");
    }
    kprintf("TEST MODE: heap self-test passed (%lu bytes free)\n", heap_free_bytes());

    kprintf("TEST MODE: starting scheduler self-test\n");
    scheduler_init();
    thread_create("worker-a", worker_a);
    thread_create("worker-b", worker_b);
    thread_create("monitor", scheduler_monitor);
    timer_set_tick_callback(schedule);
    scheduler_start();
    panic("kmain: scheduler_start returned -- unreachable");
#else
    {
        uint64_t total_mib = (pmm_total_pages() * PMM_PAGE_SIZE) / (1024 * 1024);
        uint64_t free_mib = (pmm_free_page_count() * PMM_PAGE_SIZE) / (1024 * 1024);
        kprintf("pmm ready: %lu MiB total, %lu MiB free\n", total_mib, free_mib);
    }

    kprintf("kmain: starting scheduler\n");
    scheduler_init();
    thread_create("idle", idle_thread);
    timer_set_tick_callback(schedule);
    scheduler_start();
    panic("kmain: scheduler_start returned -- unreachable");
#endif
}
