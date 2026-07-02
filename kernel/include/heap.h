#ifndef REBORNOS_HEAP_H
#define REBORNOS_HEAP_H

#include <stdint.h>

/* A first-fit free-list heap over a fixed 16 MiB physical region,
 * reserved from the PMM so nothing else can claim it. Forward
 * coalescing only on free (a freed block merges into the block right
 * after it if that one's also free, but not backward into the block
 * before it) -- a real allocator would do both directions plus
 * size-class buckets; this is the "boring, correct, obviously right"
 * version that establishes the kmalloc/kfree API for everything later
 * to build on. */
void heap_init(void);
void *kmalloc(uint64_t size);
void kfree(void *ptr);
uint64_t heap_free_bytes(void);

#endif /* REBORNOS_HEAP_H */
