#ifndef REBORNOS_SCHEDULER_H
#define REBORNOS_SCHEDULER_H

#include <stdint.h>

#define SCHEDULER_MAX_THREADS 8u
#define SCHEDULER_STACK_SIZE (16u * 1024u)

/* Simple preemptive round-robin scheduler over a fixed table of kernel
 * threads, each with its own heap-allocated stack and its own CR3.
 * Driven by the timer IRQ via timer_set_tick_callback(schedule) -- see
 * timer.h. */
void scheduler_init(void);

/* Allocates a stack and adds a new READY thread using the kernel's own
 * address space; returns its id. entry must never return (there's
 * nothing valid to return to). */
int thread_create(const char *name, void (*entry)(void));

/* Same, but the thread runs under its own address space (see
 * vmm_create_address_space()) instead of the kernel's -- for a thread
 * that's going to enter_usermode() into an isolated process. */
int thread_create_process(const char *name, void (*entry)(void), uint64_t cr3);

/* Round-robins to the next ready thread. A no-op with 0 or 1 threads.
 * Safe to call from within the timer ISR's call chain. */
void schedule(void);

/* Switches from the caller's context into the first thread and never
 * returns. Call once, after creating at least one thread. */
__attribute__((noreturn)) void scheduler_start(void);

#endif /* REBORNOS_SCHEDULER_H */
