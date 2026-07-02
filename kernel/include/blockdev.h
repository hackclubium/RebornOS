#ifndef REBORNOS_BLOCKDEV_H
#define REBORNOS_BLOCKDEV_H

#include <stdint.h>

/* The disk-agnostic interface fat16.c reads through -- today the only
 * implementation is ahci.c, but keeping the name generic (rather than
 * "ahci_read_sectors" everywhere) means fat16.c doesn't need to know
 * or care that AHCI is what's underneath, the same way vfs.c doesn't
 * know or care that FAT16 is what's underneath it. Only one disk is
 * ever supported (there's exactly one boot disk), so this is a flat
 * pair of functions rather than a registerable-device abstraction --
 * that's future work for whenever there's a second disk driver to
 * justify it. */

#define BLOCKDEV_SECTOR_SIZE 512u

/* Finds the boot disk (via PCI -- see pci.h) and gets it ready to
 * serve reads. Panics if no AHCI controller or no SATA disk is found:
 * there's no recovering from "the boot disk isn't there". */
void blockdev_init(void);

/* Reads `count` consecutive 512-byte sectors starting at `lba` into
 * buf (count * BLOCKDEV_SECTOR_SIZE bytes). Panics on a command
 * timeout or a device error -- same reasoning as blockdev_init(). */
void blockdev_read_sectors(uint64_t lba, uint32_t count, void *buf);

#endif /* REBORNOS_BLOCKDEV_H */
