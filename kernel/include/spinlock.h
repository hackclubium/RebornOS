#ifndef REBORNOS_SPINLOCK_H
#define REBORNOS_SPINLOCK_H

#include <stdint.h>

/* Every "critical section" in this kernel before SMP (kmalloc, the
 * page bitmap, ahci.c's command slot, ...) was protected with a bare
 * cli/sti pair -- correct on one core, since disabling interrupts is
 * enough to stop the *only* other thing that could run: a preempting
 * timer tick on that same core. With more than one core actually
 * executing simultaneously, cli on core 0 does nothing to stop core 1
 * from running the exact same code at the exact same time, so a real
 * cross-core lock is needed wherever more than one core can reach the
 * same shared state. Used for the shared serial port (see
 * kprintf.c/serial.c) and, since real scheduled threads now run on
 * every core (see scheduler.c), every other previously single-core-only
 * critical section too: kmalloc/kfree (heap.c), the page bitmap
 * (pmm.c), ahci.c's command slot, e1000.c's tx/rx paths, and vfs.c's
 * FAT16 operations. */
typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);

#endif /* REBORNOS_SPINLOCK_H */
