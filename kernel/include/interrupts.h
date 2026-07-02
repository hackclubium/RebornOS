#ifndef REBORNOS_INTERRUPTS_H
#define REBORNOS_INTERRUPTS_H

#include <stdint.h>

/* Matches exactly what isr_stubs.S pushes, in order (see that file for
 * why). NOTE: no rsp/ss fields -- the CPU only pushes those on a
 * privilege-level change, and everything runs in ring 0 until a later
 * milestone adds user mode. This struct will need an rsp/ss variant
 * (or a CPL check) once ring 3 exists. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags;
} interrupt_frame_t;

#define IDT_VECTOR_COUNT 34 /* CPU exceptions 0-31, IRQ0 (timer)=32, IRQ1 (keyboard)=33 */

void idt_init(void);

/* Lets device drivers (timer.c, keyboard.c, ...) claim their own IRQ
 * line without idt.c needing to know anything about the PIC or any
 * particular device -- `irq` is the PIC-relative line number (0 for
 * the timer, 1 for the keyboard, etc.), not the raw IDT vector. */
void idt_set_irq_handler(uint8_t irq, void (*handler)(interrupt_frame_t *frame));

#endif /* REBORNOS_INTERRUPTS_H */
