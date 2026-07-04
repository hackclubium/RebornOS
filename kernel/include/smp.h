#ifndef REBORNOS_SMP_H
#define REBORNOS_SMP_H

#include <stdint.h>

/* Discovers every CPU core via ACPI's MADT (see acpi.h), then boots
 * each non-BSP one (an "Application Processor") into 64-bit long mode
 * via the INIT-SIPI-SIPI sequence and a small real-mode trampoline
 * (ap_trampoline.S). Every AP finishes bringing itself up to the same
 * footing the BSP has (its own TSS, its own IDT, its own LAPIC
 * periodic timer for local preemption) and then joins the real
 * scheduler's shared ready queue -- see smp_signal_scheduler_ready()
 * for why an AP doesn't do that the instant it's done with its own
 * setup. Must run after gdt_init()/idt_init()/pmm_init()/vmm_init().
 * rsdp_addr comes from boot_info_t (see acpi.h). If ACPI discovery
 * finds nothing (or finds only the BSP), this just leaves
 * smp_cpu_count() at 1 -- SMP is a bonus, not something later boot
 * steps depend on. */
void smp_init(uint64_t rsdp_addr);

#define SMP_MAX_CPUS 16u

/* Total number of cores actually running, including the BSP (index 0). */
uint32_t smp_cpu_count(void);

/* This kernel's own 0..(smp_cpu_count()-1) core index for whichever
 * core calls it -- the BSP is always 0, APs are numbered in the order
 * they came up. Unrelated to ACPI's own APIC ID or MADT-derived
 * enumeration order. Safe to call from any core at any point after
 * smp_init() has recorded it (the BSP itself is recorded early in
 * smp_init(), before any AP is started). */
uint32_t smp_current_cpu_index(void);

/* Every AP finishes its own core-local setup and is ready to pull a
 * thread off the shared ready queue well before the BSP has actually
 * created that queue's contents -- smp_init() runs early in kmain(),
 * long before the boot-time thread_create() calls near the very end.
 * An AP that called scheduler_start() the instant it was ready would
 * find scheduler_init() hadn't even run yet. kmain() calls this right
 * after its last thread_create() and right before its own
 * scheduler_start(), and every AP blocks on it (see ap_entry() in
 * smp.c) before joining. */
void smp_signal_scheduler_ready(void);

#endif /* REBORNOS_SMP_H */
