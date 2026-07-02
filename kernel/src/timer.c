#include <stddef.h>
#include "timer.h"
#include "interrupts.h"
#include "ioport.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_FREQUENCY_HZ 1193182u

static volatile uint64_t tick_count;
static void (*tick_callback)(void);

/* IRQ0-7 default to interrupt vectors 8-15, which collide head-on with
 * CPU exception vectors (8 is Double Fault, etc.) -- every PIC-based
 * kernel must remap it before unmasking anything. offset1/offset2 are
 * the new vector bases for PIC1 (IRQ0-7) and PIC2 (IRQ8-15). */
static void pic_remap(uint8_t offset1, uint8_t offset2) {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, offset1);
    io_wait();
    outb(PIC2_DATA, offset2);
    io_wait();

    outb(PIC1_DATA, 4); /* tell PIC1 it has a secondary PIC at IRQ2 */
    io_wait();
    outb(PIC2_DATA, 2); /* tell PIC2 its cascade identity */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Mask every IRQ to start with -- each driver unmasks its own line
     * via pic_unmask_irq() once it's actually ready to handle it (see
     * keyboard_init()). */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* PIC-level primitives shared by every IRQ-driven device, not just the
 * timer -- they live here because timer.c is the module that already
 * owns PIC initialization (pic_remap() above), not because they're
 * timer-specific. */
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_unmask_irq(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t line = (irq < 8) ? irq : (uint8_t)(irq - 8);
    uint8_t mask = inb(port);
    outb(port, (uint8_t)(mask & ~(1u << line)));
}

static void pit_set_frequency(uint32_t hz) {
    uint16_t divisor = (uint16_t)(PIT_FREQUENCY_HZ / hz);
    outb(PIT_COMMAND, 0x36); /* channel 0, lobyte/hibyte access, mode 3 (square wave) */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

static void timer_irq_handler(interrupt_frame_t *frame) {
    (void)frame;
    tick_count++;
    pic_send_eoi(0);
    if (tick_callback != NULL) {
        tick_callback();
    }
}

void timer_init(uint32_t hz) {
    pic_remap(TIMER_IRQ_VECTOR, TIMER_IRQ_VECTOR + 8);
    pit_set_frequency(hz);
    idt_set_irq_handler(0, timer_irq_handler);
    pic_unmask_irq(0);
    __asm__ volatile("sti");
}

uint64_t timer_ticks(void) {
    return tick_count;
}

void timer_set_tick_callback(void (*callback)(void)) {
    tick_callback = callback;
}
