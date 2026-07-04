#include <stddef.h>
#include <stdint.h>
#include "vfs.h"
#include "fat16.h"
#include "blockdev.h"
#include "heap.h"
#include "panic.h"
#include "interrupts.h"
#include "spinlock.h"

/* fat16.c's internal state (the cached FAT, the disk block-device
 * command slot underneath it, ...) has no locking of its own -- it
 * predates real cross-core scheduling, when only one core ever ran
 * scheduled threads at all, so two filesystem calls could never
 * genuinely overlap. Now that they can, one coarse lock around every
 * public entry point here (vfs.c is the only way anything reaches
 * fat16.c) serializes the whole filesystem rather than trying to find
 * fine-grained locks inside fat16.c's several cooperating static
 * helpers -- a real bottleneck under heavy concurrent I/O, but correct,
 * and this kernel doesn't have anywhere near that kind of load yet. */
static spinlock_t fat16_vfs_lock = SPINLOCK_INIT;

void vfs_init(void) {
    blockdev_init();
    fat16_init();
}

int vfs_read_file(const char *path, void **out_buf, uint32_t *out_size) {
    uint64_t flags = irq_save_disable();
    spinlock_acquire(&fat16_vfs_lock);

    fat16_file_t file;
    if (!fat16_open(path, &file)) {
        spinlock_release(&fat16_vfs_lock);
        irq_restore(flags);
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
    spinlock_release(&fat16_vfs_lock);
    irq_restore(flags);
    return 1;
}

int vfs_read_file_into(const char *path, void *buf, uint32_t max_len) {
    uint64_t flags = irq_save_disable();
    spinlock_acquire(&fat16_vfs_lock);

    fat16_file_t file;
    if (!fat16_open(path, &file)) {
        spinlock_release(&fat16_vfs_lock);
        irq_restore(flags);
        return -1;
    }
    int result = (int)fat16_read(&file, buf, max_len);

    spinlock_release(&fat16_vfs_lock);
    irq_restore(flags);
    return result;
}

int vfs_write_file(const char *path, const void *data, uint32_t size) {
    uint64_t flags = irq_save_disable();
    spinlock_acquire(&fat16_vfs_lock);
    int result = fat16_write_file(path, data, size);
    spinlock_release(&fat16_vfs_lock);
    irq_restore(flags);
    return result;
}

int32_t vfs_list_dir(const char *path, char *buf, uint32_t max_len) {
    uint64_t flags = irq_save_disable();
    spinlock_acquire(&fat16_vfs_lock);
    int32_t result = fat16_list_dir(path, buf, max_len);
    spinlock_release(&fat16_vfs_lock);
    irq_restore(flags);
    return result;
}
