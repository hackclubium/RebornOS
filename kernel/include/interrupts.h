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

#define IDT_VECTOR_COUNT 48 /* CPU exceptions 0-31, IRQ0-15 (timer, keyboard, mouse, ...) = 32-47 */

/* Protects a short critical section against the preemptive scheduler
 * (timer_set_tick_callback(schedule) in kmain.c means a timer IRQ can
 * switch threads at any point interrupts are enabled). Unlike a bare
 * cli/sti pair, this nests correctly: if the caller is already inside
 * someone else's cli'd region (e.g. kfree() called from schedule()'s
 * own zombie-reaping critical section), irq_restore() puts IF back to
 * whatever it was on entry (still 0) instead of unconditionally
 * forcing it back on and prematurely ending the outer critical
 * section. */
static inline uint64_t irq_save_disable(void) {
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory", "cc");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile(
        "pushq %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc");
}

void idt_init(void);

/* Every core has its own IDTR register even though the IDT itself is
 * one shared table in memory -- idt_init() only loads the BSP's IDTR.
 * An AP that comes up later must call this once (after idt_init() has
 * already built the shared table) so a fault on that core doesn't
 * immediately triple-fault from having a garbage/zero IDTR. */
void idt_load_on_this_cpu(void);

/* Lets device drivers (timer.c, keyboard.c, ...) claim their own IRQ
 * line without idt.c needing to know anything about the PIC or any
 * particular device -- `irq` is the PIC-relative line number (0 for
 * the timer, 1 for the keyboard, etc.), not the raw IDT vector. */
void idt_set_irq_handler(uint8_t irq, void (*handler)(interrupt_frame_t *frame));

#endif /* REBORNOS_INTERRUPTS_H */
