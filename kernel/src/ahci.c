#include <stdint.h>
#include "blockdev.h"
#include "pci.h"
#include "pmm.h"
#include "panic.h"
#include "minilib.h"

/* A minimal polled (no interrupts) AHCI driver: enough to find the
 * boot disk and issue READ/WRITE DMA EXT commands on a single command
 * slot. No BIOS/OS handoff, no full HBA reset (OVMF's own AHCI driver
 * just used this exact controller to load our kernel, so it's already
 * in a sane state -- resetting it is unnecessary risk for zero benefit
 * here), no NCQ, no interrupts. This targets QEMU's emulated ICH9 AHCI
 * controller specifically; real hardware may need more care (spin-up
 * delays, handoff, etc). */

#define AHCI_CLASS 0x01u
#define AHCI_SUBCLASS 0x06u
#define AHCI_PROG_IF 0x01u

#define PCI_COMMAND_OFFSET 0x04u
#define PCI_COMMAND_MEMORY_SPACE (1u << 1)
#define PCI_COMMAND_BUS_MASTER (1u << 2)
#define PCI_BAR5_OFFSET 0x24u

/* HBA generic registers, offsets from ABAR. */
#define HBA_GHC 0x04u
#define HBA_PI 0x0Cu
#define GHC_AE (1u << 31)

/* Port registers, offsets from (ABAR + 0x100 + port * 0x80). */
#define PORT_REGION_BASE 0x100u
#define PORT_REGION_SIZE 0x80u
#define PORT_CLB 0x00u
#define PORT_CLBU 0x04u
#define PORT_FB 0x08u
#define PORT_FBU 0x0Cu
#define PORT_IS 0x10u
#define PORT_CMD 0x18u
#define PORT_TFD 0x20u
#define PORT_SIG 0x24u
#define PORT_SSTS 0x28u
#define PORT_SERR 0x30u
#define PORT_CI 0x38u

#define PORT_CMD_ST (1u << 0)
#define PORT_CMD_FRE (1u << 4)
#define PORT_CMD_FR (1u << 14)
#define PORT_CMD_CR (1u << 15)

#define PORT_TFD_ERR (1u << 0)

#define SATA_SIG_DISK 0x00000101u

#define FIS_TYPE_REG_H2D 0x27u
#define ATA_CMD_READ_DMA_EXT 0x25u
#define ATA_CMD_WRITE_DMA_EXT 0x35u
#define CMD_HEADER_WRITE (1u << 6) /* bit 6 of the command header's flags word: 1 = host-to-device (write) */

typedef struct __attribute__((packed)) {
    uint16_t flags;   /* bits 0-4: command FIS length in dwords; bit 6: write (0 for read) */
    uint16_t prdtl;   /* number of PRDT entries */
    uint32_t prdbc;   /* bytes transferred, filled by the HBA */
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} hba_cmd_header_t; /* 32 bytes */

typedef struct __attribute__((packed)) {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_flags; /* bits 0-21: byte count - 1 (must be odd, i.e. an even count) */
} hba_prdt_entry_t; /* 16 bytes */

typedef struct __attribute__((packed)) {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    hba_prdt_entry_t prdt[1]; /* we only ever need one entry -- see blockdev_read_sectors() */
} hba_cmd_table_t;

typedef struct __attribute__((packed)) {
    uint8_t fis_type;
    uint8_t pm_port_and_c; /* bit 7: C (this FIS updates the Command register) */
    uint8_t command;
    uint8_t feature_low;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t feature_high;
    uint8_t count_low, count_high;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} fis_reg_h2d_t; /* 20 bytes */

static uintptr_t hba_base;
static uintptr_t port_base;
static hba_cmd_header_t *cmd_list;
static hba_cmd_table_t *cmd_table;

static uint32_t hba_read32(uint32_t offset) {
    return *(volatile uint32_t *)(hba_base + offset);
}

static void hba_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(hba_base + offset) = value;
}

static uint32_t port_read32(uint32_t offset) {
    return *(volatile uint32_t *)(port_base + offset);
}

static void port_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(port_base + offset) = value;
}

static void port_stop(void) {
    port_write32(PORT_CMD, port_read32(PORT_CMD) & ~(PORT_CMD_ST | PORT_CMD_FRE));

    uint64_t spins = 0;
    while (port_read32(PORT_CMD) & (PORT_CMD_FR | PORT_CMD_CR)) {
        spins++;
        if (spins > 100000000ULL) {
            panic("ahci: port didn't stop its command engine in time");
        }
    }
}

static void port_start(void) {
    uint64_t spins = 0;
    while (port_read32(PORT_CMD) & PORT_CMD_CR) {
        spins++;
        if (spins > 100000000ULL) {
            panic("ahci: port's command engine still running before restart");
        }
    }
    port_write32(PORT_CMD, port_read32(PORT_CMD) | PORT_CMD_FRE | PORT_CMD_ST);
}

void blockdev_init(void) {
    pci_device_t dev;
    if (!pci_find_device(AHCI_CLASS, AHCI_SUBCLASS, AHCI_PROG_IF, &dev)) {
        panic("ahci: no AHCI controller found on PCI bus 0");
    }

    uint32_t command_reg = pci_config_read32(dev, PCI_COMMAND_OFFSET);
    command_reg |= PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
    pci_config_write32(dev, PCI_COMMAND_OFFSET, command_reg);

    /* BAR5 (ABAR) is a 32-bit memory BAR for every AHCI controller QEMU
     * emulates; bits 0-3 are flags, not part of the address. */
    hba_base = (uintptr_t)(pci_config_read32(dev, PCI_BAR5_OFFSET) & ~0xFu);

    hba_write32(HBA_GHC, hba_read32(HBA_GHC) | GHC_AE);

    uint32_t ports_implemented = hba_read32(HBA_PI);
    int found_port = -1;
    for (int i = 0; i < 32; i++) {
        if (!(ports_implemented & (1u << i))) {
            continue;
        }
        uintptr_t candidate = hba_base + PORT_REGION_BASE + (uint32_t)i * PORT_REGION_SIZE;
        uint32_t ssts = *(volatile uint32_t *)(candidate + PORT_SSTS);
        uint32_t det = ssts & 0xFu;
        uint32_t sig = *(volatile uint32_t *)(candidate + PORT_SIG);
        if (det == 3 && sig == SATA_SIG_DISK) {
            found_port = i;
            break;
        }
    }
    if (found_port < 0) {
        panic("ahci: no SATA disk found on any implemented port");
    }
    port_base = hba_base + PORT_REGION_BASE + (uint32_t)found_port * PORT_REGION_SIZE;

    port_stop();

    /* One page each for the command list and FIS receive area -- both
     * need to be 1KB/256B aligned respectively, and a whole 4KiB page
     * trivially satisfies either, so there's no need for a dedicated
     * aligned allocator. Physical == virtual here, same as everywhere
     * else in this kernel (see vmm.c's identity map). */
    void *clb = pmm_alloc_page();
    void *fb = pmm_alloc_page();
    void *ctb = pmm_alloc_page();
    if (clb == 0 || fb == 0 || ctb == 0) {
        panic("ahci: out of physical memory for command structures");
    }
    memset(clb, 0, PMM_PAGE_SIZE);
    memset(fb, 0, PMM_PAGE_SIZE);
    memset(ctb, 0, PMM_PAGE_SIZE);

    cmd_list = (hba_cmd_header_t *)clb;
    cmd_table = (hba_cmd_table_t *)ctb;

    port_write32(PORT_CLB, (uint32_t)(uintptr_t)clb);
    port_write32(PORT_CLBU, 0);
    port_write32(PORT_FB, (uint32_t)(uintptr_t)fb);
    port_write32(PORT_FBU, 0);

    /* Command slot 0 is the only one this driver ever uses. */
    cmd_list[0].ctba = (uint32_t)(uintptr_t)ctb;
    cmd_list[0].ctbau = 0;

    port_write32(PORT_SERR, port_read32(PORT_SERR)); /* clear any pending error bits (W1C) */
    port_start();
}

/* Shared by both directions: reads and writes only differ in the ATA
 * command byte and the command header's Write bit -- the PRDT either
 * side uses just names a physical buffer the HBA DMAs into or out of. */
static void issue_rw_command(uint64_t lba, uint32_t count, void *buf, int is_write) {
    if (count == 0) {
        return;
    }

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pm_port_and_c = 0x80; /* C bit set -- this FIS issues a command */
    fis->command = is_write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    fis->device = 0x40; /* LBA mode */
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->count_low = (uint8_t)(count & 0xFF);
    fis->count_high = (uint8_t)((count >> 8) & 0xFF);

    cmd_table->prdt[0].dba = (uint32_t)(uintptr_t)buf;
    cmd_table->prdt[0].dbau = 0;
    cmd_table->prdt[0].dbc_flags = (count * BLOCKDEV_SECTOR_SIZE - 1) & 0x3FFFFFu;

    cmd_list[0].flags = (uint16_t)(sizeof(fis_reg_h2d_t) / 4) | (uint16_t)(is_write ? CMD_HEADER_WRITE : 0);
    cmd_list[0].prdtl = 1;
    cmd_list[0].prdbc = 0;

    port_write32(PORT_CI, 1u << 0);

    uint64_t spins = 0;
    while (port_read32(PORT_CI) & 1u) {
        spins++;
        if (spins > 200000000ULL) {
            panic("ahci: command timed out (lba=%lu count=%u write=%d)", lba, count, is_write);
        }
    }

    uint32_t tfd = port_read32(PORT_TFD);
    if (tfd & PORT_TFD_ERR) {
        panic("ahci: device reported an error (lba=%lu count=%u write=%d, tfd=0x%lx)",
              lba, count, is_write, (uint64_t)tfd);
    }
}

void blockdev_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    issue_rw_command(lba, count, buf, 0);
}

void blockdev_write_sectors(uint64_t lba, uint32_t count, const void *buf) {
    issue_rw_command(lba, count, (void *)(uintptr_t)buf, 1);
}
