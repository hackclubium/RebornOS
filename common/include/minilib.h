#ifndef REBORNOS_MINILIB_H
#define REBORNOS_MINILIB_H

#include <stddef.h>

/* Freestanding builds have no libc. GCC still emits implicit calls to
 * memset/memcpy/memmove for struct assignment and array init, so these
 * must exist with exactly these signatures for the linker to resolve
 * them. strlen/strcmp are added because kprintf and boot logging need
 * them. */

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

#endif /* REBORNOS_MINILIB_H */
