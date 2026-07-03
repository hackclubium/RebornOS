#ifndef REBORNOS_SMP_H
#define REBORNOS_SMP_H

#include <stdint.h>

/* Discovers every CPU core via ACPI's MADT (see acpi.h), then boots
 * each non-BSP one (an "Application Processor") into 64-bit long mode
 * via the INIT-SIPI-SIPI sequence and a small real-mode trampoline
 * (ap_trampoline.S). Every AP just spins incrementing its own private
 * counter (smp_ap_counters[]) to prove it's genuinely running in
 * parallel with the BSP -- distributing real scheduled threads across
 * cores is future work, not this milestone. Must run after
 * gdt_init()/idt_init()/pmm_init()/vmm_init(). rsdp_addr comes from
 * boot_info_t (see acpi.h). If ACPI discovery finds nothing (or finds
 * only the BSP), this just leaves smp_cpu_count() at 1 -- SMP is a
 * bonus, not something later boot steps depend on. */
void smp_init(uint64_t rsdp_addr);

#define SMP_MAX_CPUS 16u

/* Total number of cores actually running, including the BSP (index 0). */
uint32_t smp_cpu_count(void);

/* smp_ap_counters[i] is core i's own private counter, incremented only
 * by that core (index 0, the BSP, is incremented by kmain.c's own
 * self-test thread, not by smp.c) -- since no other core ever writes
 * to a given slot, no lock is needed to read or write it. */
extern volatile uint64_t smp_ap_counters[SMP_MAX_CPUS];

#endif /* REBORNOS_SMP_H */
