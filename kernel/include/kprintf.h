#ifndef REBORNOS_KPRINTF_H
#define REBORNOS_KPRINTF_H

#include <stdarg.h>

/* Serial-only logger. Supports %s %c %d %u %x %lx %p %%. There is no
 * libc here, so this is a small hand-rolled formatter, not a real
 * printf -- unrecognized specifiers are printed literally. */
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list args);

#endif /* REBORNOS_KPRINTF_H */
