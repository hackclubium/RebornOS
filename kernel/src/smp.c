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
#include "scheduler.h"

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

static uint32_t cpu_count = 1;
static uint8_t apic_ids[SMP_MAX_CPUS];
static uint32_t apic_id_count;

/* This kernel's own core numbering -- unrelated to ACPI's enumeration
 * order in apic_ids[]. The BSP is unconditionally index 0 (gdt_init()
 * and everything else that runs before smp_init() has even executed
 * needs a stable answer with no lookup possible yet); each AP that
 * successfully starts gets the next index in start order. Recorded
 * here (by the BSP, which already knows both values the moment an AP
 * confirms it's up) so any core can later ask "which index am I"
 * via smp_current_cpu_index() by matching its own LAPIC ID against
 * this table, without needing per-CPU storage this kernel doesn't
 * have. */
static uint32_t core_apic_ids[SMP_MAX_CPUS];

/* Set by the BSP right before an AP's SIPI, read once by that AP early
 * in ap_entry() -- safe as a single reused slot for the same reason
 * ap_cr3/ap_stack_top/ap_entry_point are: bring-up is strictly one AP
 * at a time. */
static volatile uint32_t ap_core_index;

/* Bumped by an AP once it's done enough of its own setup to be safely
 * left running while the BSP moves on to starting the next one --
 * volatile, and safe to spin-poll without a lock: x86's cache
 * coherency alone is enough for this "wait until a flag changes"
 * pattern, no explicit fence needed. */
static volatile uint32_t ap_started_count;

/* Set once by the BSP, right before its own scheduler_start(), after
 * every boot-time thread_create() call has already run -- see
 * smp_signal_scheduler_ready()'s doc comment in smp.h for why an AP
 * can't just join the instant its own setup is done. */
static volatile int scheduler_ready;

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
 * every kernel thread expects (its own TSS, its own IDT, its own LAPIC
 * timer for local preemption), then joins the shared scheduler exactly
 * the way the BSP does -- scheduler_start() never returns, so nothing
 * after that call ever runs. */
void ap_entry(void) {
    uint32_t my_index = ap_core_index; /* set by the BSP right before this AP's SIPI */

    gdt_load_on_this_cpu(my_index);
    idt_load_on_this_cpu();
    lapic_init();
    lapic_timer_start();

    __asm__ volatile("" ::: "memory"); /* make sure setup above is visible before we signal readiness */
    __sync_fetch_and_add(&ap_started_count, 1);

    while (!scheduler_ready) {
        __asm__ volatile("pause");
    }
    scheduler_start();
}

void smp_signal_scheduler_ready(void) {
    __asm__ volatile("" ::: "memory"); /* every thread_create() call above must be visible first */
    scheduler_ready = 1;
}

void smp_init(uint64_t rsdp_addr) {
    if (!acpi_find_local_apic_ids(rsdp_addr, apic_ids, SMP_MAX_CPUS, &apic_id_count)) {
        kprintf("smp: no ACPI MADT found -- running single-core\n");
        return;
    }

    lapic_init();
    uint32_t bsp_apic_id = lapic_id();
    core_apic_ids[0] = bsp_apic_id;
    uint32_t sipi_vector = (uint32_t)((uintptr_t)ap_trampoline_start >> 12);

    /* Every core's LAPIC runs off the same bus clock, so this one
     * measurement (on the BSP, before any AP exists) is all every AP's
     * later lapic_timer_start() call needs. */
    lapic_calibrate_timer();

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
        ap_core_index = cpu_count; /* the index this AP will get if it comes up */

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
            core_apic_ids[cpu_count] = apic_ids[i];
            cpu_count++;
        }
    }

    kprintf("smp: %u core(s) running\n", cpu_count);
}

uint32_t smp_cpu_count(void) {
    return cpu_count;
}

uint32_t smp_current_cpu_index(void) {
    uint32_t id = lapic_id();
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (core_apic_ids[i] == id) {
            return i;
        }
    }
    return 0; /* BSP fallback: called before smp_init() has run, or genuinely single-core */
}
