#ifndef REBORNOS_PANIC_H
#define REBORNOS_PANIC_H

/* Prints a panic message and a best-effort return-address stack trace to
 * serial, paints a panic screen on the framebuffer, signals failure to
 * QEMU (see qemu_debug.h), and halts. Never returns. */
__attribute__((noreturn)) void panic(const char *fmt, ...);

#endif /* REBORNOS_PANIC_H */
