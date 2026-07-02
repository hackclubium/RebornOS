#ifndef REBORNOS_SERIAL_H
#define REBORNOS_SERIAL_H

/* Minimal 16550 UART driver on COM1 (port 0x3F8). Shared verbatim between
 * the bootloader and the kernel -- it's pure port I/O, so the same C file
 * compiles cleanly under both the PE (mingw) and ELF cross-compilers. */

void serial_init(void);
void serial_write_char(char c);
void serial_write(const char *s);

#endif /* REBORNOS_SERIAL_H */
