#include <stddef.h>
#include <stdint.h>
#include "process.h"
#include "vfs.h"
#include "elf_loader.h"
#include "vmm.h"
#include "scheduler.h"
#include "heap.h"
#include "panic.h"

int process_spawn(const char *path, const char *thread_name) {
    void *elf_data = NULL;
    uint32_t elf_size = 0;
    if (!vfs_read_file(path, &elf_data, &elf_size)) {
        return -1;
    }

    uint64_t cr3 = vmm_create_address_space();
    elf_process_t *proc = (elf_process_t *)kmalloc(sizeof(elf_process_t));
    if (proc == NULL) {
        panic("process_spawn: kmalloc failed for launch info");
    }
    elf_load_user_program((const uint8_t *)elf_data, elf_size, cr3, proc);
    kfree(elf_data);

    return thread_create_process_arg(thread_name, elf_launch_process, proc, cr3);
}

int process_spawn_and_wait(const char *path, const char *thread_name) {
    int tid = process_spawn(path, thread_name);
    if (tid < 0) {
        return -1;
    }
    while (thread_is_alive(tid)) {
        schedule();
    }
    return 0;
}
