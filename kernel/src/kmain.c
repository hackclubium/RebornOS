#include <stddef.h>
#include "boot_info.h"
#include "serial.h"
#include "framebuffer.h"
#include "panic.h"
#include "kprintf.h"
#include "qemu_debug.h"

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
    fb_puts(8, 8, "REBORNOS -- MILESTONE 0: COLD BOOT", 0xFFFFFFFF, 0x00102030);
    fb_puts(8, 24, "SERIAL + FRAMEBUFFER ALIVE.", 0xFFA0A0A0, 0x00102030);

    kprintf("kmain: boot checks complete\n");

#ifdef REBORNOS_TEST_MODE
    kprintf("TEST MODE: reaching this line without panicking is the test\n");
    qemu_debug_exit(QEMU_DEBUG_EXIT_SUCCESS);
#endif

    kprintf("kmain: halting (no scheduler yet -- that's the next milestone)\n");
    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}
