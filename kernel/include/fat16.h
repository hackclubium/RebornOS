#ifndef REBORNOS_FAT16_H
#define REBORNOS_FAT16_H

#include <stdint.h>

/* A read-only FAT16 driver over the real boot disk, read sector by
 * sector through blockdev.h (see ahci.c) -- no more in-RAM disk image.
 *
 * FAT16, not FAT32: QEMU's own vvfat driver documents its FAT32 mode as
 * untested, and it really is broken here -- forcing it produced a
 * volume OVMF's boot manager couldn't even find. FAT16 is the format
 * vvfat actually supports reliably, and its root directory is simpler
 * to read anyway: a fixed-size area right after the FATs, not a
 * cluster chain like FAT32's. Root directory only, no writing, no
 * subdirectories -- the minimum needed to find and load a handful of
 * files off the ESP. */

typedef struct {
    uint32_t first_cluster;
    uint32_t size; /* bytes */
} fat16_file_t;

/* Reads the boot sector via blockdev_read_sectors() and panics if it
 * doesn't look like a valid FAT16 volume -- a malformed boot sector
 * means nothing downstream (VFS, ELF loading) can work, so failing
 * loudly here beats a mysterious crash three layers up. Also reads and
 * caches the whole FAT in RAM (small -- FAT16 tops out around 128KiB),
 * so cluster-chain walks don't need a disk read per step. */
void fat16_init(void);

/* Looks up `name` (case-insensitive, matched against the FAT 8.3 short
 * name -- e.g. "init.elf" matches an on-disk "INIT    ELF") in the root
 * directory only. Returns 1 and fills *out on success, 0 if not found. */
int fat16_open(const char *name, fat16_file_t *out);

/* Reads up to max_len bytes of `file` into buf, following its cluster
 * chain. Returns the number of bytes actually copied. */
uint32_t fat16_read(const fat16_file_t *file, void *buf, uint32_t max_len);

/* Fills buf with every root-directory filename, one per line, formatted
 * as "NAME.EXT" (padding stripped, dot omitted for extension-less
 * names) -- for the shell's `ls`. Always NUL-terminates within max_len
 * (truncating the listing if it doesn't fit) and returns the number of
 * bytes written, not counting the NUL. */
uint32_t fat16_list_root(char *buf, uint32_t max_len);

#endif /* REBORNOS_FAT16_H */
