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

#define IDT_VECTOR_COUNT 33 /* CPU exceptions 0-31, plus IRQ0 (timer) at 32 */

void idt_init(void);

/* Lets timer.c (or later, other device drivers) claim vectors >= 32
 * without idt.c needing to know anything about the PIC. */
void idt_set_irq_handler(void (*handler)(interrupt_frame_t *frame));

#endif /* REBORNOS_INTERRUPTS_H */
