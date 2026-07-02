#include <stdint.h>
#include "pci.h"
#include "ioport.h"

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA    0xCFCu

static uint32_t pci_address(pci_device_t dev, uint8_t offset) {
    return (1u << 31) | ((uint32_t)dev.bus << 16) | ((uint32_t)dev.device << 11) |
           ((uint32_t)dev.function << 8) | (offset & 0xFCu);
}

uint32_t pci_config_read32(pci_device_t dev, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(dev, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(pci_device_t dev, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(dev, offset));
    outl(PCI_CONFIG_DATA, value);
}

int pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out) {
    for (uint16_t device = 0; device < 32; device++) {
        for (uint16_t function = 0; function < 8; function++) {
            pci_device_t dev = { .bus = 0, .device = (uint8_t)device, .function = (uint8_t)function };

            uint32_t id = pci_config_read32(dev, 0x00);
            if ((id & 0xFFFFu) == 0xFFFFu) {
                continue; /* vendor ID 0xFFFF -- nothing here */
            }

            /* Offset 0x08: [31:24] class, [23:16] subclass, [15:8] prog-if, [7:0] revision. */
            uint32_t class_reg = pci_config_read32(dev, 0x08);
            uint8_t dev_class = (uint8_t)(class_reg >> 24);
            uint8_t dev_subclass = (uint8_t)(class_reg >> 16);
            uint8_t dev_prog_if = (uint8_t)(class_reg >> 8);

            if (dev_class == class_code && dev_subclass == subclass && dev_prog_if == prog_if) {
                *out = dev;
                return 1;
            }

            if (function == 0) {
                /* Offset 0x0C: [23:16] header type, bit 7 = multi-function.
                 * If it's not set, functions 1-7 of this device don't
                 * exist -- skip straight to the next device. */
                uint32_t header_reg = pci_config_read32(dev, 0x0C);
                uint8_t header_type = (uint8_t)(header_reg >> 16);
                if ((header_type & 0x80u) == 0) {
                    break;
                }
            }
        }
    }
    return 0;
}
