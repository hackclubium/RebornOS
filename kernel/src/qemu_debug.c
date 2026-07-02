#include "qemu_debug.h"
#include "ioport.h"

void qemu_debug_exit(uint8_t code) {
    outb(0xf4, code);
    /* If we're not running under QEMU with isa-debug-exit wired up (e.g.
     * on real hardware later), the port write above just goes nowhere,
     * so fall back to halting instead of falling off the end. */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
