#include <stdint.h>
#include "mouse.h"
#include "interrupts.h"
#include "timer.h"
#include "ioport.h"
#include "framebuffer.h"

#define PS2_DATA 0x60u
#define PS2_STATUS 0x64u
#define PS2_CMD 0x64u

#define PS2_STATUS_OUT_FULL (1u << 0)
#define PS2_STATUS_IN_FULL (1u << 1)

#define MOUSE_IRQ 12u

/* Bits in a mouse packet's first byte. */
#define PACKET_LEFT_BTN (1u << 0)
#define PACKET_RIGHT_BTN (1u << 1)
#define PACKET_MIDDLE_BTN (1u << 2)
#define PACKET_ALWAYS_ONE (1u << 3) /* lets a desynced byte stream be detected and dropped */
#define PACKET_X_SIGN (1u << 4)
#define PACKET_Y_SIGN (1u << 5)

static int32_t mouse_x;
static int32_t mouse_y;
static uint8_t mouse_buttons;
static uint8_t packet[3];
static uint32_t packet_index;

/* A real (or emulated) 8042 controller responds in microseconds, so
 * this ceiling only exists to bound how long a genuinely wedged
 * controller can stall boot -- it used to be 100,000,000, which is
 * high enough that a full, unlucky handshake (several of these calls
 * each maxing out) could burn tens of seconds of real time under host
 * contention (e.g. many QEMU instances launched back-to-back in a
 * stress-test loop), long enough to trip test-qemu.sh's 40s timeout
 * and look exactly like a hung kernel. Lower by 100x: still enormously
 * generous for real hardware, but caps the worst case at a fraction of
 * a second instead. */
#define PS2_WAIT_SPIN_LIMIT 1000000ULL

static void wait_input_clear(void) {
    uint64_t spins = 0;
    while (inb(PS2_STATUS) & PS2_STATUS_IN_FULL) {
        spins++;
        if (spins > PS2_WAIT_SPIN_LIMIT) {
            return; /* controller wedged -- give up rather than hang the whole boot */
        }
        __asm__ volatile("pause");
    }
}

static void wait_output_full(void) {
    uint64_t spins = 0;
    while (!(inb(PS2_STATUS) & PS2_STATUS_OUT_FULL)) {
        spins++;
        if (spins > PS2_WAIT_SPIN_LIMIT) {
            return;
        }
        __asm__ volatile("pause");
    }
}

static void ps2_write_command(uint8_t cmd) {
    wait_input_clear();
    outb(PS2_CMD, cmd);
}

static void ps2_write_data(uint8_t data) {
    wait_input_clear();
    outb(PS2_DATA, data);
}

static uint8_t ps2_read_data(void) {
    wait_output_full();
    return inb(PS2_DATA);
}

/* 0xD4 tells the 8042 controller "the next byte on the data port is
 * for the auxiliary (mouse) device, not the keyboard". */
static void mouse_write(uint8_t data) {
    ps2_write_command(0xD4u);
    ps2_write_data(data);
}

static void mouse_irq_handler(interrupt_frame_t *frame) {
    (void)frame;
    uint8_t data = inb(PS2_DATA);
    pic_send_eoi(MOUSE_IRQ);

    packet[packet_index++] = data;
    if (packet_index < 3) {
        return;
    }
    packet_index = 0;

    uint8_t flags = packet[0];
    if (!(flags & PACKET_ALWAYS_ONE)) {
        return; /* desynced byte stream -- drop this packet and hope the next one realigns */
    }

    int32_t dx = packet[1];
    int32_t dy = packet[2];
    if (flags & PACKET_X_SIGN) {
        dx -= 256;
    }
    if (flags & PACKET_Y_SIGN) {
        dy -= 256;
    }

    mouse_buttons = flags & (PACKET_LEFT_BTN | PACKET_RIGHT_BTN | PACKET_MIDDLE_BTN);

    mouse_x += dx;
    mouse_y -= dy; /* PS/2's Y axis increases upward; the framebuffer's increases downward */

    const boot_framebuffer_t *fb = fb_get_info();
    if (mouse_x < 0) {
        mouse_x = 0;
    }
    if (mouse_y < 0) {
        mouse_y = 0;
    }
    if (fb->width > 0 && mouse_x >= (int32_t)fb->width) {
        mouse_x = (int32_t)fb->width - 1;
    }
    if (fb->height > 0 && mouse_y >= (int32_t)fb->height) {
        mouse_y = (int32_t)fb->height - 1;
    }
}

void mouse_init(void) {
    /* cli for the whole handshake: IRQ1 (keyboard) is already unmasked
     * and interrupts are already globally enabled by this point in
     * boot (timer_init() turned them on), and the 8042 controller's
     * "read/write configuration byte" responses land in the same
     * output buffer a keyboard byte would -- without this, a stray
     * IRQ1 firing mid-handshake can steal the response this function
     * is polling for, right out from under it (this is exactly what
     * happened before: the config-byte read alone would time out,
     * while the mouse's own 0xD4-prefixed command ACKs -- sent before
     * IRQ12 is unmasked below -- never raced with anything and always
     * came back fine). Safe to hold this long: mouse_init() runs
     * during single-threaded boot, before the scheduler exists. */
    __asm__ volatile("cli");

    ps2_write_command(0xA8u); /* enable the auxiliary (mouse) device */

    ps2_write_command(0x20u); /* read controller configuration byte */
    uint8_t config = ps2_read_data();
    config |= (1u << 1);  /* enable IRQ12 */
    config &= (uint8_t)~(1u << 5); /* enable the aux device's own clock */
    ps2_write_command(0x60u); /* write controller configuration byte */
    ps2_write_data(config);

    mouse_write(0xF6u); /* set defaults */
    (void)ps2_read_data(); /* ACK (0xFA) -- polled reads only, nothing to validate it against yet */
    mouse_write(0xF4u); /* enable data reporting */
    (void)ps2_read_data(); /* ACK */

    mouse_x = 0;
    mouse_y = 0;
    mouse_buttons = 0;
    packet_index = 0;

    idt_set_irq_handler(MOUSE_IRQ, mouse_irq_handler);
    pic_unmask_irq(MOUSE_IRQ);
    /* IRQ12 is a secondary-PIC line (IRQ8-15) -- it only ever reaches
     * the CPU by cascading through IRQ2 on the primary PIC, which
     * pic_remap() masked along with everything else and nothing had
     * unmasked yet (keyboard.c only ever needed IRQ1, on the primary
     * PIC directly). */
    pic_unmask_irq(2);

    __asm__ volatile("sti");
}

void mouse_get_state(mouse_state_t *out) {
    __asm__ volatile("cli");
    out->x = mouse_x;
    out->y = mouse_y;
    out->buttons = mouse_buttons;
    __asm__ volatile("sti");
}
