#include <stdint.h>
#include "e1000.h"
#include "pci.h"
#include "pmm.h"
#include "panic.h"
#include "minilib.h"
#include "interrupts.h"
#include "spinlock.h"

#define E1000_CLASS 0x02u    /* network controller */
#define E1000_SUBCLASS 0x00u /* ethernet controller */
#define E1000_PROG_IF 0x00u

#define PCI_COMMAND_OFFSET 0x04u
#define PCI_COMMAND_MEMORY_SPACE (1u << 1)
#define PCI_COMMAND_BUS_MASTER (1u << 2)
#define PCI_BAR0_OFFSET 0x10u

/* Register offsets from the memory-mapped BAR0 -- see the Intel 8254x
 * Software Developer's Manual. Only what a minimal polled driver
 * actually touches. */
#define REG_CTRL   0x0000u
#define REG_STATUS 0x0008u
#define REG_ICR    0x00C0u
#define REG_IMS    0x00D0u
#define REG_IMC    0x00D8u
#define REG_RCTL   0x0100u
#define REG_TCTL   0x0400u
#define REG_TIPG   0x0410u
#define REG_RDBAL  0x2800u
#define REG_RDBAH  0x2804u
#define REG_RDLEN  0x2808u
#define REG_RDH    0x2810u
#define REG_RDT    0x2818u
#define REG_TDBAL  0x3800u
#define REG_TDBAH  0x3804u
#define REG_TDLEN  0x3808u
#define REG_TDH    0x3810u
#define REG_TDT    0x3818u
#define REG_RAL0   0x5400u
#define REG_RAH0   0x5404u

#define CTRL_ASDE (1u << 5)
#define CTRL_SLU  (1u << 6)  /* Set Link Up */
#define CTRL_RST  (1u << 26)

#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15) /* Broadcast Accept Mode */
#define RCTL_BSIZE_2048 0u     /* with BSEX=0, BSIZE=00 means 2048-byte buffers */
#define RCTL_SECRC  (1u << 26) /* strip the Ethernet CRC before it reaches our buffer */

#define TCTL_EN  (1u << 1)
#define TCTL_PSP (1u << 3) /* Pad Short Packets */
#define TCTL_CT_SHIFT 4
#define TCTL_COLD_SHIFT 12

#define TX_CMD_EOP  (1u << 0) /* End Of Packet */
#define TX_CMD_IFCS (1u << 1) /* Insert FCS */
#define TX_CMD_RS   (1u << 3) /* Report Status -- set DD once sent */

#define DESC_STATUS_DD (1u << 0) /* Descriptor Done -- same bit position in both RX and TX status bytes */

#define E1000_BUF_SIZE 2048u /* matches RCTL_BSIZE_2048 above */
#define RX_DESC_COUNT 8u     /* 8 * 16 bytes == 128 bytes, exactly the required ring alignment */
#define TX_DESC_COUNT 8u

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t; /* 16 bytes */

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t; /* 16 bytes */

static uintptr_t mmio_base;
/* volatile: the card fills in each descriptor's status/length/checksum
 * fields asynchronously via DMA -- without volatile, the compiler
 * could hoist a repeated desc->status read out of the polling loops
 * below and spin on a stale cached value forever. */
static volatile e1000_rx_desc_t *rx_ring;
static volatile e1000_tx_desc_t *tx_ring;
static uint32_t rx_tail;   /* next descriptor we'll hand back to the card once drained */
static uint32_t tx_tail;   /* next descriptor we'll fill for the next send */
static uint8_t mac[6];

static uint32_t reg_read(uint32_t offset) {
    return *(volatile uint32_t *)(mmio_base + offset);
}

static void reg_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(mmio_base + offset) = value;
}

void e1000_init(void) {
    pci_device_t dev;
    if (!pci_find_device(E1000_CLASS, E1000_SUBCLASS, E1000_PROG_IF, &dev)) {
        panic("e1000: no ethernet controller found on PCI bus 0");
    }

    uint32_t command_reg = pci_config_read32(dev, PCI_COMMAND_OFFSET);
    command_reg |= PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
    pci_config_write32(dev, PCI_COMMAND_OFFSET, command_reg);

    /* BAR0 is a 32-bit memory BAR for the e1000; bits 0-3 are flags,
     * not part of the address (same convention as ahci.c's ABAR). */
    mmio_base = (uintptr_t)(pci_config_read32(dev, PCI_BAR0_OFFSET) & ~0xFu);

    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    uint64_t spins = 0;
    while (reg_read(REG_CTRL) & CTRL_RST) {
        spins++;
        if (spins > 100000000ULL) {
            panic("e1000: device didn't come out of reset in time");
        }
    }

    /* This driver is polled -- mask every interrupt and drain whatever
     * cause bits reset happened to leave set. */
    reg_write(REG_IMC, 0xFFFFFFFFu);
    (void)reg_read(REG_ICR);

    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    /* QEMU pre-populates RAL0/RAH0 from `-device e1000,mac=...` (or a
     * default if none was given), so reading them back after reset is
     * enough to learn our own MAC -- no EEPROM read protocol needed. */
    uint32_t ral = reg_read(REG_RAL0);
    uint32_t rah = reg_read(REG_RAH0);
    mac[0] = (uint8_t)(ral & 0xFFu);
    mac[1] = (uint8_t)((ral >> 8) & 0xFFu);
    mac[2] = (uint8_t)((ral >> 16) & 0xFFu);
    mac[3] = (uint8_t)((ral >> 24) & 0xFFu);
    mac[4] = (uint8_t)(rah & 0xFFu);
    mac[5] = (uint8_t)((rah >> 8) & 0xFFu);

    /* One physical page trivially satisfies both rings' size (128
     * bytes needed) and alignment requirements, same reasoning ahci.c
     * uses for its command structures -- no dedicated aligned
     * allocator needed. */
    rx_ring = (volatile e1000_rx_desc_t *)pmm_alloc_page();
    tx_ring = (volatile e1000_tx_desc_t *)pmm_alloc_page();
    if (rx_ring == NULL || tx_ring == NULL) {
        panic("e1000: out of physical memory for the descriptor rings");
    }
    memset((void *)rx_ring, 0, PMM_PAGE_SIZE);
    memset((void *)tx_ring, 0, PMM_PAGE_SIZE);

    for (uint32_t i = 0; i < RX_DESC_COUNT; i++) {
        void *buf = pmm_alloc_page();
        if (buf == NULL) {
            panic("e1000: out of physical memory for an RX buffer");
        }
        rx_ring[i].addr = (uint64_t)(uintptr_t)buf;
        rx_ring[i].status = 0;
    }
    for (uint32_t i = 0; i < TX_DESC_COUNT; i++) {
        void *buf = pmm_alloc_page();
        if (buf == NULL) {
            panic("e1000: out of physical memory for a TX buffer");
        }
        tx_ring[i].addr = (uint64_t)(uintptr_t)buf;
        tx_ring[i].status = 0;
    }

    reg_write(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    reg_write(REG_RDBAH, 0);
    reg_write(REG_RDLEN, RX_DESC_COUNT * (uint32_t)sizeof(e1000_rx_desc_t));
    reg_write(REG_RDH, 0);
    /* RDT is exclusive of the slot it points at -- leaving one
     * descriptor "owned by software" here is what keeps a full ring
     * distinguishable from an empty one. */
    reg_write(REG_RDT, RX_DESC_COUNT - 1);
    rx_tail = RX_DESC_COUNT - 1;
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

    reg_write(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    reg_write(REG_TDBAH, 0);
    reg_write(REG_TDLEN, TX_DESC_COUNT * (uint32_t)sizeof(e1000_tx_desc_t));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    tx_tail = 0;
    /* CT=15 (collision threshold), COLD=64 (collision distance, full
     * duplex value) -- standard values straight out of the datasheet's
     * example configuration. */
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));
    reg_write(REG_TIPG, 10u | (8u << 10) | (6u << 20)); /* IPGT/IPGR1/IPGR2, full-duplex defaults */
}

void e1000_get_mac(uint8_t mac_out[6]) {
    memcpy(mac_out, mac, 6);
}

/* tx_tail/rx_tail and the descriptor rings are shared global state
 * with no per-thread copies -- same hazard as ahci.c's single command
 * slot, and the same fix: a lock around each ring's own tail-advancing
 * sequence, since two cores both calling e1000_send() (or both calling
 * e1000_poll_receive()) at once would otherwise race to claim the same
 * descriptor slot and corrupt each other's bookkeeping. cli isn't
 * needed here in addition to the lock for e1000_poll_receive() (it
 * never blocks), but e1000_send()'s poll loop can spin for a while, so
 * it keeps the same irq_save_disable()/lock pairing as ahci.c/heap.c
 * to stay consistent with how every other shared-hardware-state
 * section in this kernel is protected. */
static spinlock_t e1000_tx_lock = SPINLOCK_INIT;
static spinlock_t e1000_rx_lock = SPINLOCK_INIT;

void e1000_send(const void *frame, uint16_t len) {
    if (len > E1000_BUF_SIZE) {
        panic("e1000_send: frame too large (%u bytes, max %u)", (unsigned)len, E1000_BUF_SIZE);
    }

    uint64_t flags = irq_save_disable();
    spinlock_acquire(&e1000_tx_lock);

    volatile e1000_tx_desc_t *desc = &tx_ring[tx_tail];
    memcpy((void *)(uintptr_t)desc->addr, frame, len);
    desc->length = len;
    desc->cso = 0;
    desc->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    desc->status = 0;

    tx_tail = (tx_tail + 1) % TX_DESC_COUNT;
    reg_write(REG_TDT, tx_tail);

    uint64_t spins = 0;
    while (!(desc->status & DESC_STATUS_DD)) {
        spins++;
        if (spins > 100000000ULL) {
            panic("e1000_send: card never reported the frame as sent");
        }
    }

    spinlock_release(&e1000_tx_lock);
    irq_restore(flags);
}

uint16_t e1000_poll_receive(void *buf, uint16_t max_len) {
    uint64_t flags = irq_save_disable();
    spinlock_acquire(&e1000_rx_lock);

    volatile e1000_rx_desc_t *desc = &rx_ring[(rx_tail + 1) % RX_DESC_COUNT];
    if (!(desc->status & DESC_STATUS_DD)) {
        spinlock_release(&e1000_rx_lock);
        irq_restore(flags);
        return 0; /* nothing new */
    }

    uint16_t len = desc->length;
    uint16_t copy_len = len < max_len ? len : max_len;
    memcpy(buf, (const void *)(uintptr_t)desc->addr, copy_len);

    desc->status = 0;
    rx_tail = (rx_tail + 1) % RX_DESC_COUNT;
    reg_write(REG_RDT, rx_tail);

    spinlock_release(&e1000_rx_lock);
    irq_restore(flags);
    return len;
}
