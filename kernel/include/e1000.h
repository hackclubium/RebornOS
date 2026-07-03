#ifndef REBORNOS_E1000_H
#define REBORNOS_E1000_H

#include <stdint.h>

/* A minimal polled (no interrupts) driver for the Intel 82540EM
 * ("e1000"), the NIC QEMU emulates by default for `-device e1000` --
 * same reasoning as ahci.c: enough to find the card, learn our own MAC
 * address, and send/receive whole Ethernet frames on a small fixed
 * ring, with no interrupt-driven completion, no VLANs, no checksum
 * offload, no multiple queues. */

/* Finds the NIC via PCI (class 0x02 network, subclass 0x00 ethernet),
 * enables it, and brings up a small RX/TX descriptor ring. Panics if
 * no ethernet controller is found -- there's no recovering from "the
 * NIC isn't there" any more than there is for the boot disk. */
void e1000_init(void);

/* Copies our own 6-byte MAC address (read back from the card's own
 * RAL0/RAH0 registers, which QEMU pre-populates from `-device
 * e1000,mac=...`) into mac_out. */
void e1000_get_mac(uint8_t mac_out[6]);

/* Sends one Ethernet frame (dst MAC + src MAC + ethertype + payload,
 * all already assembled by the caller -- this layer doesn't know
 * anything about ARP/IP/etc). Blocks (polling the TX descriptor's
 * Descriptor Done bit) until the card has actually taken it. Panics if
 * `len` doesn't fit in one descriptor's buffer or the card never
 * finishes sending. */
void e1000_send(const void *frame, uint16_t len);

/* Non-blocking: copies the next received frame (if any) into buf (up
 * to max_len bytes) and returns its real length, or 0 if nothing has
 * arrived yet. Callers that want to wait for a specific reply loop on
 * this themselves (see net.c), the same way SYS_READ_CHAR loops on
 * keyboard_read_char(). */
uint16_t e1000_poll_receive(void *buf, uint16_t max_len);

#endif /* REBORNOS_E1000_H */
