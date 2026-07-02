#ifndef REBORNOS_IOPORT_H
#define REBORNOS_IOPORT_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* A write to an unused port (0x80 is a POST-code port nothing else on
 * a PC uses) takes long enough to act as a short delay -- old PIC/PIT
 * initialization sequences rely on this between writes. */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif /* REBORNOS_IOPORT_H */
