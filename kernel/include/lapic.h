#ifndef REBORNOS_LAPIC_H
#define REBORNOS_LAPIC_H

#include <stdint.h>
#include "interrupts.h"

/* Local APIC access -- used to send the INIT-SIPI-SIPI sequence that
 * starts another CPU core (see smp.c), and to give every AP its own
 * periodic timer interrupt for local preemption. The BSP keeps using
 * the legacy 8259 PIC/PIT exactly as before (see timer.c/keyboard.c)
 * -- the legacy PIC only ever delivers to the BSP, so it can't drive
 * preemption on an AP at all; IPIs and the LAPIC's own timer are a
 * separate mechanism that doesn't touch or conflict with that. */

/* The vector this kernel's LAPIC periodic timer fires on -- outside
 * the legacy IRQ0-15 range (32-47) idt.c's interrupt_dispatch() routes
 * through irq_handlers[], since a LAPIC-sourced interrupt needs its
 * own EOI (written to the LAPIC, not the legacy PIC) and is handled
 * directly by lapic_timer_isr() instead. */
#define LAPIC_TIMER_VECTOR 48u

/* Enables this CPU's Local APIC (reading its MMIO base from the
 * IA32_APIC_BASE MSR) and its own spurious-interrupt vector -- must be
 * called once per core that will ever send or receive an IPI. */
void lapic_init(void);

/* This core's own Local APIC ID. */
uint32_t lapic_id(void);

/* Sends an INIT IPI to the CPU with the given target APIC ID -- the
 * first step of waking up an Application Processor. Blocks until the
 * card reports the IPI as delivered. */
void lapic_send_init(uint8_t target_apic_id);

/* Sends a Startup IPI (SIPI) to the given target, telling it to begin
 * executing in real mode at physical address (vector << 12) -- the
 * second and third steps of the INIT-SIPI-SIPI sequence (sent twice,
 * per the MP startup protocol). Blocks until delivered. */
void lapic_send_sipi(uint8_t target_apic_id, uint8_t vector);

/* Measures the LAPIC timer's own tick rate against the legacy PIT (via
 * timer_ticks(), see timer.h) and remembers the initial-count value
 * that yields roughly the same interrupt rate timer_init() already
 * programmed the PIT for -- must run exactly once, on the BSP, after
 * lapic_init() and after timer_init() (interrupts must already be
 * enabled so timer_ticks() is actually advancing). Every core's LAPIC
 * runs off the same bus clock, so the one measurement is reused by
 * every subsequent lapic_timer_start() call, BSP or AP, with no need
 * to recalibrate per core. */
void lapic_calibrate_timer(void);

/* Starts this core's own periodic LAPIC timer on LAPIC_TIMER_VECTOR,
 * using the initial count lapic_calibrate_timer() already measured.
 * Called by every AP (see ap_entry() in smp.c) right after lapic_init()
 * -- the BSP never calls this, since it already has a working
 * preemption source via the legacy PIT/PIC. */
void lapic_timer_start(void);

/* Vector 48's handler (see interrupt_dispatch() in idt.c): calls
 * schedule() and issues a LAPIC EOI. Not registered through
 * idt_set_irq_handler() like a legacy IRQ -- idt.c calls it directly,
 * the same way it special-cases the syscall vector. */
void lapic_timer_isr(interrupt_frame_t *frame);

#endif /* REBORNOS_LAPIC_H */
