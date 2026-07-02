#include <stddef.h>
#include <stdint.h>
#include "vfs.h"
#include "fat16.h"
#include "blockdev.h"
#include "heap.h"
#include "panic.h"

void vfs_init(void) {
    blockdev_init();
    fat16_init();
}

int vfs_read_file(const char *path, void **out_buf, uint32_t *out_size) {
    fat16_file_t file;
    if (!fat16_open(path, &file)) {
        return 0;
    }

    void *buf = kmalloc(file.size);
    if (buf == NULL) {
        panic("vfs_read_file: kmalloc(%lu) failed for '%s'", (uint64_t)file.size, path);
    }

    uint32_t read = fat16_read(&file, buf, file.size);
    if (read != file.size) {
        panic("vfs_read_file: short read on '%s' (%lu of %lu bytes)", path, (uint64_t)read, (uint64_t)file.size);
    }

    *out_buf = buf;
    *out_size = file.size;
    return 1;
}

uint32_t vfs_list_root(char *buf, uint32_t max_len) {
    return fat16_list_root(buf, max_len);
}
