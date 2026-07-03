#ifndef REBORNOS_VFS_H
#define REBORNOS_VFS_H

#include <stdint.h>

/* The thinnest possible VFS: exactly one filesystem is ever mounted
 * (the FAT16 volume on the boot disk -- see fat16.h), so there is no
 * mount table and no path-to-filesystem routing -- just a handful of
 * whole-file operations. A real VFS (multiple mounts, open file
 * descriptors, incremental reads) is future work once there's more
 * than one filesystem to route between. */
void vfs_init(void);

/* Reads the whole file at `path` into a freshly kmalloc'd buffer.
 * Returns 1 and fills out_buf/out_size on success, 0 if the file
 * doesn't exist. Caller owns the returned buffer and must kfree() it. */
int vfs_read_file(const char *path, void **out_buf, uint32_t *out_size);

/* Reads up to max_len bytes of the file at `path` into a caller-
 * supplied buffer (no allocation) -- for a syscall handing a fixed
 * user buffer straight through. Returns the number of bytes read, or
 * -1 if the file doesn't exist. */
int vfs_read_file_into(const char *path, void *buf, uint32_t max_len);

/* Creates or overwrites the file at `path` (root directory only -- see
 * fat16.h's scope cuts) with `size` bytes from `data`. Returns 1 on
 * success. */
int vfs_write_file(const char *path, const void *data, uint32_t size);

/* Fills buf with the filenames in the directory at `path` (an empty
 * string or NULL means the root directory), one per line -- see
 * fat16_list_dir(). Returns the number of bytes written, or -1 if
 * `path` isn't a directory. */
int32_t vfs_list_dir(const char *path, char *buf, uint32_t max_len);

#endif /* REBORNOS_VFS_H */
