#ifndef REBORNOS_LAPIC_H
#define REBORNOS_LAPIC_H

#include <stdint.h>

/* Local APIC access -- used only to send the INIT-SIPI-SIPI sequence
 * that starts another CPU core (see smp.c). This kernel keeps routing
 * every actual hardware IRQ (timer, keyboard) through the legacy 8259
 * PIC exactly as before (see timer.c/keyboard.c) -- IPIs are a
 * separate mechanism from PIC-routed interrupts, so enabling the LAPIC
 * for this doesn't touch or conflict with that at all. */

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

#endif /* REBORNOS_LAPIC_H */
