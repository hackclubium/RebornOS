#include <stdint.h>
#include <stddef.h>
#include "keyboard.h"
#include "interrupts.h"
#include "timer.h"
#include "ioport.h"

#define KEYBOARD_IRQ 1
#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_BUFFER_SIZE 64

/* US QWERTY, scancode set 1, make codes only -- break codes (key
 * released) are >= 0x80 and are dropped before ever reaching this
 * table. Indexed directly by scancode; 0 means "no printable
 * character" (modifiers, function keys, unmapped slots). Shift is
 * deliberately not tracked -- see keyboard.h. */
static const char scancode_to_ascii[0x3A] = {
    /* 0x00 */ 0, 0 /* ESC */, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    /* 0x0F */ '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    /* 0x1D */ 0 /* lctrl */, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    /* 0x2A */ 0 /* lshift */, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    /* 0x36 */ 0 /* rshift */, '*', 0 /* lalt */, ' ',
};

static char buffer[KEYBOARD_BUFFER_SIZE];
static uint32_t buf_head;
static uint32_t buf_tail;

static void buffer_push(char c) {
    uint32_t next = (buf_head + 1) % KEYBOARD_BUFFER_SIZE;
    if (next == buf_tail) {
        return; /* buffer full -- drop the keystroke rather than clobber an unread one */
    }
    buffer[buf_head] = c;
    buf_head = next;
}

static void keyboard_irq_handler(interrupt_frame_t *frame) {
    (void)frame;
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    pic_send_eoi(KEYBOARD_IRQ);

    if ((scancode & 0x80) != 0) {
        return; /* break code -- nothing to do without modifier-key tracking */
    }
    if (scancode < sizeof(scancode_to_ascii)) {
        char c = scancode_to_ascii[scancode];
        if (c != 0) {
            buffer_push(c);
        }
    }
}

void keyboard_init(void) {
    idt_set_irq_handler(KEYBOARD_IRQ, keyboard_irq_handler);
    pic_unmask_irq(KEYBOARD_IRQ);
}

int keyboard_read_char(void) {
    __asm__ volatile("cli");
    int result = -1;
    if (buf_tail != buf_head) {
        result = (unsigned char)buffer[buf_tail];
        buf_tail = (buf_tail + 1) % KEYBOARD_BUFFER_SIZE;
    }
    __asm__ volatile("sti");
    return result;
}

void keyboard_inject_char(char c) {
    __asm__ volatile("cli");
    buffer_push(c);
    __asm__ volatile("sti");
}
