#ifndef REBORNOS_VFS_H
#define REBORNOS_VFS_H

#include <stdint.h>
#include "boot_info.h"

/* The thinnest possible VFS: exactly one filesystem is ever mounted
 * (the FAT16 volume the bootloader read into RAM -- see fat32.h), so
 * there is no mount table, no path-to-filesystem routing, and no open
 * file descriptors -- just "give me the whole contents of this file".
 * A real VFS (multiple mounts, directories, incremental reads) is
 * future work once there's more than one filesystem to route between. */
void vfs_init(const boot_info_t *info);

/* Reads the whole file at `path` (an 8.3 name in the root directory --
 * see fat32.h's scope cuts) into a freshly kmalloc'd buffer. Returns 1
 * and fills out_buf/out_size on success, 0 if the file doesn't exist.
 * Caller owns the returned buffer and must kfree() it. */
int vfs_read_file(const char *path, void **out_buf, uint32_t *out_size);

#endif /* REBORNOS_VFS_H */
