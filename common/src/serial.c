#include <stdint.h>
#include "serial.h"

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* disable UART interrupts, we poll */
    outb(COM1 + 3, 0x80); /* enable DLAB to set the baud divisor */
    outb(COM1 + 0, 0x03); /* divisor low byte: 115200 / 3 = 38400 baud */
    outb(COM1 + 1, 0x00); /* divisor high byte */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit; clears DLAB */
    outb(COM1 + 2, 0xC7); /* enable FIFO, clear it, 14-byte threshold */
    outb(COM1 + 4, 0x0B); /* IRQs enabled (unused), RTS/DSR set */
}

static int transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_write_char(char c) {
    while (!transmit_empty()) {
    }
    outb(COM1, (uint8_t)c);
    if (c == '\n') {
        while (!transmit_empty()) {
        }
        outb(COM1, '\r');
    }
}

void serial_write(const char *s) {
    while (*s) {
        serial_write_char(*s++);
    }
}
