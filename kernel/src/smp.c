#include <stddef.h>
#include <stdint.h>
#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "interrupts.h"
#include "gdt.h"
#include "vmm.h"
#include "heap.h"
#include "panic.h"
#include "kprintf.h"

#define AP_STACK_SIZE (16u * 1024u)
#define AP_START_TIMEOUT_SPINS 200000000ULL

/* Defined in ap_trampoline.S, linked into the .ap_trampoline section
 * at a fixed physical address (see kernel/linker.ld) -- ordinary
 * extern globals, not a separately-copied blob, since that section is
 * already part of this same kernel.elf link. smp.c fills these in
 * before triggering each AP's SIPI; bring-up is strictly one-AP-at-a-
 * time, so reusing the same three slots for every AP is safe. */
extern const uint8_t ap_trampoline_start[];
extern volatile uint64_t ap_cr3;
extern volatile uint64_t ap_stack_top;
extern volatile uint64_t ap_entry_point;

volatile uint64_t smp_ap_counters[SMP_MAX_CPUS];
static uint32_t cpu_count = 1;
static uint8_t apic_ids[SMP_MAX_CPUS];
static uint32_t apic_id_count;

/* Bumped by an AP once it's done enough of its own setup to be safely
 * left running while the BSP moves on to starting the next one --
 * volatile, and safe to spin-poll without a lock: x86's cache
 * coherency alone is enough for this "wait until a flag changes"
 * pattern, no explicit fence needed. */
static volatile uint32_t ap_started_count;

static void spin_delay(uint64_t iterations) {
    for (uint64_t i = 0; i < iterations; i++) {
        __asm__ volatile("pause");
    }
}

/* Reached via ap_trampoline.S's final `call *%rax` -- the CPU is
 * already in 64-bit long mode with paging on (using the kernel's own
 * page tables) and running on its own real stack, but still pointed at
 * the trampoline's temporary GDT and whatever garbage IDTR real mode
 * left behind. Finishes bringing this core up to the same footing
 * every kernel thread expects, then proves it's alive by spinning on
 * its own private counter forever -- distributing real scheduled
 * threads across cores is future work. */
void ap_entry(void) {
    gdt_load_on_this_cpu();
    idt_load_on_this_cpu();
    lapic_init();

    uint32_t my_apic_id = lapic_id();
    uint32_t my_index = 0;
    for (uint32_t i = 0; i < apic_id_count; i++) {
        if (apic_ids[i] == my_apic_id) {
            my_index = i;
            break;
        }
    }

    __asm__ volatile("" ::: "memory"); /* make sure setup above is visible before we signal readiness */
    __sync_fetch_and_add(&ap_started_count, 1);

    for (;;) {
        smp_ap_counters[my_index]++;
    }
}

void smp_init(uint64_t rsdp_addr) {
    if (!acpi_find_local_apic_ids(rsdp_addr, apic_ids, SMP_MAX_CPUS, &apic_id_count)) {
        kprintf("smp: no ACPI MADT found -- running single-core\n");
        return;
    }

    lapic_init();
    uint32_t bsp_apic_id = lapic_id();
    uint32_t sipi_vector = (uint32_t)((uintptr_t)ap_trampoline_start >> 12);

    for (uint32_t i = 0; i < apic_id_count; i++) {
        if (apic_ids[i] == bsp_apic_id) {
            continue; /* that's us -- already running */
        }

        void *stack = kmalloc(AP_STACK_SIZE);
        if (stack == NULL) {
            panic("smp_init: kmalloc failed for an AP stack");
        }

        ap_cr3 = vmm_kernel_cr3();
        ap_stack_top = (uint64_t)(uintptr_t)stack + AP_STACK_SIZE;
        ap_entry_point = (uint64_t)(uintptr_t)ap_entry;

        uint32_t before = ap_started_count;

        /* Standard MP startup sequence: INIT, a short delay, then SIPI
         * sent twice with a shorter delay between -- real hardware and
         * QEMU alike expect this exact shape. */
        lapic_send_init(apic_ids[i]);
        spin_delay(10000000ULL);
        lapic_send_sipi(apic_ids[i], (uint8_t)sipi_vector);
        spin_delay(2000000ULL);
        lapic_send_sipi(apic_ids[i], (uint8_t)sipi_vector);

        uint64_t spins = 0;
        while (ap_started_count == before) {
            spins++;
            if (spins > AP_START_TIMEOUT_SPINS) {
                kprintf("smp: CPU (APIC ID %u) never came up -- continuing without it\n",
                        (unsigned)apic_ids[i]);
                break;
            }
            __asm__ volatile("pause");
        }
        if (ap_started_count != before) {
            cpu_count++;
        }
    }

    kprintf("smp: %u core(s) running\n", cpu_count);
}

uint32_t smp_cpu_count(void) {
    return cpu_count;
}
