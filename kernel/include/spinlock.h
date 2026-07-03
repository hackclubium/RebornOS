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
 * same shared state. This first lock protects the shared serial port
 * (see kprintf.c/serial.c) -- e1000.c, heap.c, pmm.c, and friends stay
 * single-core-only for this milestone (only the BSP runs scheduled
 * threads; the APs just spin incrementing their own private counters),
 * so they don't need one yet. */
typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);

#endif /* REBORNOS_SPINLOCK_H */
