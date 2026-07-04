#include <stdint.h>
#include <stddef.h>
#include "interrupts.h"
#include "panic.h"
#include "elf_loader.h"
#include "scheduler.h"
#include "kprintf.h"
#include "lapic.h"
#include "smp.h"

extern uint64_t isr_stub_table[IDT_VECTOR_COUNT];
extern void isr_stub_128(void); /* int $0x80 -- registered directly, not part of isr_stub_table */
extern void syscall_dispatch(interrupt_frame_t *frame);

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

#define IDT_ENTRY_COUNT 256
#define IDT_TYPE_INTERRUPT_GATE      0x8E /* present, ring 0, 64-bit interrupt gate */
#define IDT_TYPE_INTERRUPT_GATE_DPL3 0xEE /* same, but DPL=3 so `int $0x80` from ring 3 doesn't #GP */
#define SYSCALL_VECTOR 128

static idt_entry_t idt[IDT_ENTRY_COUNT];

static inline uint16_t read_cs(void) {
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void idt_set_entry(int vector, uint64_t handler, uint16_t selector, uint8_t type_attr) {
    idt[vector].offset_low = (uint16_t)(handler & 0xFFFF);
    idt[vector].selector = selector;
    idt[vector].ist = 0;
    idt[vector].type_attr = type_attr;
    idt[vector].offset_mid = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)(handler >> 32);
    idt[vector].reserved = 0;
}

static const char *exception_name(uint64_t vector) {
    static const char *names[32] = {
        "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
        "BOUND Range Exceeded", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
        "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
        "Page Fault", "Reserved", "x87 FP Exception", "Alignment Check",
        "Machine Check", "SIMD FP Exception", "Virtualization Exception",
        "Control Protection Exception", "Reserved", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
        "VMM Communication Exception", "Security Exception",
    };
    return (vector < 32) ? names[vector] : "Unknown";
}

/* Fault-isolation exit codes live at -1000 and below, well clear of
 * both 0 (clean exit) and -1 (process_spawn_and_wait()'s "no such
 * program" sentinel) and of any small nonzero code a program might
 * legitimately pass to SYS_EXIT itself. */
#define FAULT_EXIT_CODE(vector) (-1000 - (int64_t)(vector))

static void exception_handler(interrupt_frame_t *frame) {
    int from_ring3 = (frame->cs & 0x3) == 3;
    /* Diagnostic only: which physical core and which of our own thread
     * records was executing when this fault happened. rip/error_code
     * alone don't say that, and it matters a lot for telling apart "the
     * BSP's PIT-driven schedule() path" from "an AP's LAPIC-driven
     * one" while chasing an intermittent, core-dependent bug. */
    uint32_t core = smp_current_cpu_index();
    const char *tname = thread_current_name();

    if (frame->vector == 14) {
        uint64_t fault_addr = read_cr2();
        if (elf_handle_stack_fault(fault_addr, frame->error_code, from_ring3)) {
            return; /* demand-paged in -- iretq retries the faulting instruction */
        }
        if (from_ring3) {
            kprintf("fault: process killed by page fault at rip=0x%lx, error_code=0x%lx, "
                    "fault address (cr2)=0x%lx, core=%u, thread=\"%s\"\n",
                    frame->rip, frame->error_code, fault_addr, core, tname);
            thread_exit_code(FAULT_EXIT_CODE(frame->vector));
        }
        panic("CPU exception %lu (%s) at rip=0x%lx, error_code=0x%lx, fault address (cr2)=0x%lx, "
              "core=%u, thread=\"%s\"",
              frame->vector, exception_name(frame->vector), frame->rip, frame->error_code, fault_addr,
              core, tname);
    }

    /* Any other exception from ring 3 (invalid opcode, GPF, divide
     * error, ...) ends just the offending process, not the kernel --
     * an exception taken from ring 0 is our own bug, and stays fatal
     * so it's never silently swallowed. */
    if (from_ring3) {
        kprintf("fault: process killed by CPU exception %lu (%s) at rip=0x%lx, error_code=0x%lx, "
                "core=%u, thread=\"%s\"\n",
                frame->vector, exception_name(frame->vector), frame->rip, frame->error_code, core, tname);
        thread_exit_code(FAULT_EXIT_CODE(frame->vector));
    }

    /* Diagnostic only, chasing an intermittent ring-0 rip=0 crash: dump
     * a window of stack memory around the interrupted context's own
     * rbp (captured by isr_common_stub before anything in the C
     * dispatch chain touches it) to tell apart "one corrupted return
     * address" from "this whole region reads as zero", which would
     * point at some memset() landing on a live kernel stack instead of
     * its intended freshly allocated page. 8-byte-aligned, non-NULL
     * check only: this is already a fatal path, so a bad read here just
     * produces a second, still-informative fault. */
    if (frame->rbp != 0 && (frame->rbp & 0x7) == 0) {
        uint64_t *p = (uint64_t *)(uintptr_t)frame->rbp;
        kprintf("diag: rbp=0x%lx stack window [rbp-32..rbp+32]: "
                "%lx %lx %lx %lx %lx %lx %lx %lx %lx\n",
                frame->rbp, p[-4], p[-3], p[-2], p[-1], p[0], p[1], p[2], p[3], p[4]);
    } else {
        kprintf("diag: rbp=0x%lx (NULL or misaligned -- skipping stack window dump)\n", frame->rbp);
    }

    panic("CPU exception %lu (%s) at rip=0x%lx, error_code=0x%lx, core=%u, thread=\"%s\"",
          frame->vector, exception_name(frame->vector), frame->rip, frame->error_code, core, tname);
}

/* IRQs (vector >= 32) are wired up by individual device drivers
 * (timer.c, keyboard.c, ...), indexed by PIC-relative IRQ line rather
 * than raw vector so idt.c doesn't need to know about the PIC's
 * remap offset. Sized for IRQ0-15 (both legacy PICs), even though only
 * two lines are actually used today. */
#define IRQ_HANDLER_COUNT 16
static void (*irq_handlers[IRQ_HANDLER_COUNT])(interrupt_frame_t *frame);

void idt_set_irq_handler(uint8_t irq, void (*handler)(interrupt_frame_t *frame)) {
    if (irq < IRQ_HANDLER_COUNT) {
        irq_handlers[irq] = handler;
    }
}

void interrupt_dispatch(interrupt_frame_t *frame) {
    if (frame->vector < 32) {
        exception_handler(frame);
        return;
    }
    if (frame->vector == SYSCALL_VECTOR) {
        syscall_dispatch(frame);
        return;
    }
    if (frame->vector == LAPIC_TIMER_VECTOR) {
        lapic_timer_isr(frame);
        return;
    }
    if (frame->vector >= 32 && frame->vector < 32 + IRQ_HANDLER_COUNT) {
        uint8_t irq = (uint8_t)(frame->vector - 32);
        if (irq_handlers[irq] != NULL) {
            irq_handlers[irq](frame);
        }
    }
}

void idt_init(void) {
    uint16_t cs = read_cs(); /* gdt_init() must have already run, so this is our own kernel CS */

    for (int i = 0; i < IDT_VECTOR_COUNT; i++) {
        idt_set_entry(i, isr_stub_table[i], cs, IDT_TYPE_INTERRUPT_GATE);
    }
    idt_set_entry(SYSCALL_VECTOR, (uint64_t)(uintptr_t)isr_stub_128, cs, IDT_TYPE_INTERRUPT_GATE_DPL3);

    idt_load_on_this_cpu();
}

void idt_load_on_this_cpu(void) {
    idt_ptr_t idt_ptr = {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)(uintptr_t)&idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}
