#ifndef REBORNOS_TIMER_H
#define REBORNOS_TIMER_H

#include <stdint.h>

#define TIMER_IRQ_VECTOR 32 /* IRQ0 after the PIC remap in timer_init() */

/* Remaps the legacy 8259 PIC off the CPU exception vector range,
 * masks every IRQ except the timer, programs the PIT to fire at
 * `hz`, and registers the tick handler with the IDT. Call after
 * idt_init(). */
void timer_init(uint32_t hz);
uint64_t timer_ticks(void);

/* Registers a function to run on every tick, after EOI -- this is how
 * the scheduler (see scheduler.h) drives preemption without timer.c
 * needing to know it exists. */
void timer_set_tick_callback(void (*callback)(void));

#endif /* REBORNOS_TIMER_H */
