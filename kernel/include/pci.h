#ifndef REBORNOS_PCI_H
#define REBORNOS_PCI_H

#include <stdint.h>

/* Legacy PCI configuration space access (CONFIG_ADDRESS/CONFIG_DATA at
 * ports 0xCF8/0xCFC) -- works on every x86 PC including QEMU's q35
 * machine, no ACPI MCFG/ECAM parsing needed for a plain 256-byte
 * config space read, which is all a driver like ahci.c needs. */

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
} pci_device_t;

uint32_t pci_config_read32(pci_device_t dev, uint8_t offset);
void pci_config_write32(pci_device_t dev, uint8_t offset, uint32_t value);

/* Scans bus 0 (QEMU's q35 puts every device we care about there --
 * recursing into PCI-to-PCI bridges on secondary buses is future work,
 * not needed to find the ICH9 AHCI controller) for the first device
 * whose class/subclass/prog-if match. Returns 1 and fills *out on
 * success, 0 if nothing matched. */
int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out);

#endif /* REBORNOS_PCI_H */
