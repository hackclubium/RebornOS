#ifndef REBORNOS_FAT16_H
#define REBORNOS_FAT16_H

#include <stdint.h>

/* A FAT16 driver over the real boot disk, read/written sector by
 * sector through blockdev.h (see ahci.c).
 *
 * FAT16, not FAT32: QEMU's own vvfat driver documents its FAT32 mode as
 * untested, and it really is broken here -- forcing it produced a
 * volume OVMF's boot manager couldn't even find. FAT16 is the format
 * vvfat actually supports reliably, and its root directory is simpler
 * to read anyway: a fixed-size area right after the FATs, not a
 * cluster chain like FAT32's.
 *
 * Subdirectories can be read and traversed (multi-component paths like
 * "SUBDIR/FILE.ELF" work for opening, listing, and running programs),
 * but writing is root-directory only -- creating or growing a
 * subdirectory's own cluster chain is future work, so fat16_write_file()
 * only ever touches the fixed root area. */

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

/* Looks up `path` (case-insensitive 8.3 components separated by '/',
 * e.g. "SUBDIR/FILE.ELF" or a bare "FILE.ELF" for the root directory).
 * Returns 1 and fills *out on success, 0 if any component doesn't
 * exist. */
int fat16_open(const char *path, fat16_file_t *out);

/* Reads up to max_len bytes of `file` into buf, following its cluster
 * chain. Returns the number of bytes actually copied. */
uint32_t fat16_read(const fat16_file_t *file, void *buf, uint32_t max_len);

/* Fills buf with every filename in the directory at `path` (an empty
 * string or NULL means the root directory), one per line, formatted as
 * "NAME.EXT" (padding stripped, dot omitted for extension-less names).
 * Always NUL-terminates within max_len (truncating the listing if it
 * doesn't fit) and returns the number of bytes written (not counting
 * the NUL), or -1 if `path` doesn't resolve to a directory. */
int32_t fat16_list_dir(const char *path, char *buf, uint32_t max_len);

/* Creates or overwrites a file named `name` (a plain 8.3 name, root
 * directory only -- see the scope note above) with `size` bytes from
 * `data`. Returns 1 on success; panics if the disk or root directory
 * is genuinely full, since there's no graceful way to partially write
 * a file. */
int fat16_write_file(const char *name, const void *data, uint32_t size);

#endif /* REBORNOS_FAT16_H */
