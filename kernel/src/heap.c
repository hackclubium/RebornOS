#include <stddef.h>
#include "heap.h"
#include "pmm.h"
#include "panic.h"
#include "interrupts.h"

#define HEAP_BASE 0x1000000ULL       /* 16 MiB physical/virtual (identity-mapped) */
#define HEAP_SIZE (16ULL * 1024 * 1024)
#define HEAP_ALIGN 16u

typedef struct heap_block {
    uint64_t size; /* usable bytes after this header, not counting the header itself */
    int free;
    struct heap_block *next;
} heap_block_t;

static heap_block_t *heap_head;

void heap_init(void) {
    pmm_reserve_region(HEAP_BASE, HEAP_SIZE);

    heap_head = (heap_block_t *)(uintptr_t)HEAP_BASE;
    heap_head->size = HEAP_SIZE - sizeof(heap_block_t);
    heap_head->free = 1;
    heap_head->next = NULL;
}

static void split_block(heap_block_t *block, uint64_t size) {
    /* Only split off a new free block if the leftover is big enough to
     * be worth tracking on its own (header + a little usable space);
     * otherwise leave the extra few bytes as internal fragmentation. */
    if (block->size < size + sizeof(heap_block_t) + HEAP_ALIGN) {
        return;
    }
    heap_block_t *remainder = (heap_block_t *)((uint8_t *)block + sizeof(heap_block_t) + size);
    remainder->size = block->size - size - sizeof(heap_block_t);
    remainder->free = 1;
    remainder->next = block->next;

    block->size = size;
    block->next = remainder;
}

/* The free list (heap_head and every block's size/free/next) is global
 * mutable state with no per-thread copies, and the scheduler is
 * preemptive -- a timer tick can land mid-split_block() or mid-kfree()
 * and switch to another thread that calls kmalloc/kfree itself,
 * walking or mutating a half-updated list. irq_save_disable()/
 * irq_restore() make each call atomic with respect to preemption;
 * kmalloc/kfree are short and bounded, so this is a cheap, correct fix
 * rather than needing a real lock. Must nest correctly (not a bare
 * cli/sti pair) since both are called from other already-cli'd
 * critical sections, e.g. schedule()'s zombie-reaping kfree(). */
void *kmalloc(uint64_t size) {
    if (size == 0) {
        return NULL;
    }
    size = (size + (HEAP_ALIGN - 1)) & ~(uint64_t)(HEAP_ALIGN - 1);

    uint64_t flags = irq_save_disable();
    for (heap_block_t *b = heap_head; b != NULL; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = 0;
            irq_restore(flags);
            return (void *)((uint8_t *)b + sizeof(heap_block_t));
        }
    }
    irq_restore(flags);
    return NULL;
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    uint64_t flags = irq_save_disable();
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));
    block->free = 1;

    if (block->next != NULL && block->next->free) {
        block->size += sizeof(heap_block_t) + block->next->size;
        block->next = block->next->next;
    }
    irq_restore(flags);
}

uint64_t heap_free_bytes(void) {
    uint64_t total = 0;
    for (heap_block_t *b = heap_head; b != NULL; b = b->next) {
        if (b->free) {
            total += b->size;
        }
    }
    return total;
}
