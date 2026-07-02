#include <stdint.h>
#include <stddef.h>
#include "interrupts.h"
#include "panic.h"

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

static void exception_handler(interrupt_frame_t *frame) {
    if (frame->vector == 14) {
        panic("CPU exception %lu (%s) at rip=0x%lx, error_code=0x%lx, fault address (cr2)=0x%lx",
              frame->vector, exception_name(frame->vector), frame->rip, frame->error_code, read_cr2());
    }
    panic("CPU exception %lu (%s) at rip=0x%lx, error_code=0x%lx",
          frame->vector, exception_name(frame->vector), frame->rip, frame->error_code);
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

    idt_ptr_t idt_ptr = {
        .limit = sizeof(idt) - 1,
        .base = (uint64_t)(uintptr_t)&idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}
