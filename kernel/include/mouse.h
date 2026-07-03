#ifndef REBORNOS_MOUSE_H
#define REBORNOS_MOUSE_H

#include <stdint.h>

/* A minimal PS/2 mouse driver: standard 3-byte packet protocol, no
 * scroll wheel (Intellimouse extension), no more than one device.
 * Tracks an absolute (x, y) position clamped to the framebuffer's own
 * bounds (see framebuffer.h) rather than exposing raw relative deltas
 * -- simpler for a first driver, and it's what a cursor/window manager
 * actually wants anyway.
 *
 * Registers its own IRQ12 handler -- call after keyboard_init() (both
 * share the same 8042 controller, and this needs the PIC already
 * remapped -- see timer_init()'s ordering comment in kmain.c). */
void mouse_init(void);

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t buttons; /* bit 0 = left, bit 1 = right, bit 2 = middle */
} mouse_state_t;

/* Copies out the current position/button state. Never blocks --
 * there's no "new data available" concept here, just the latest
 * known state, updated asynchronously by the IRQ handler. */
void mouse_get_state(mouse_state_t *out);

#endif /* REBORNOS_MOUSE_H */
