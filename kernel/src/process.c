#include <stddef.h>
#include <stdint.h>
#include "process.h"
#include "vfs.h"
#include "elf_loader.h"
#include "vmm.h"
#include "scheduler.h"
#include "heap.h"
#include "panic.h"

static int process_spawn_args_ex(const char *path, const char *thread_name, int argc, char **argv,
                                  int64_t *exit_code_out) {
    void *elf_data = NULL;
    uint32_t elf_size = 0;
    if (!vfs_read_file(path, &elf_data, &elf_size)) {
        return -1;
    }

    uint64_t cr3 = vmm_create_address_space();
    elf_process_t *proc = (elf_process_t *)kmalloc(sizeof(elf_process_t));
    if (proc == NULL) {
        panic("process_spawn_args: kmalloc failed for launch info");
    }
    elf_load_user_program((const uint8_t *)elf_data, elf_size, cr3, argc, argv, proc);
    kfree(elf_data);

    return thread_create_process_arg(thread_name, elf_launch_process, proc, cr3, exit_code_out);
}

int process_spawn_args(const char *path, const char *thread_name, int argc, char **argv) {
    return process_spawn_args_ex(path, thread_name, argc, argv, NULL);
}

int process_spawn(const char *path, const char *thread_name) {
    return process_spawn_args(path, thread_name, 0, NULL);
}

int process_spawn_args_and_wait(const char *path, const char *thread_name, int argc, char **argv) {
    /* Lives in this call's own stack frame, which stays alive for the
     * whole wait below -- thread_exit_code() writes through this
     * pointer the instant the child exits, so there's no window where
     * an unrelated thread_create_ex() call could reuse the child's
     * slot and clobber its exit code before it's read (see the
     * exit_code_out comment in scheduler.c). */
    int64_t exit_code = 0;
    int tid = process_spawn_args_ex(path, thread_name, argc, argv, &exit_code);
    if (tid < 0) {
        return -1;
    }
    while (thread_is_alive(tid)) {
        schedule();
    }
    /* The child's own SYS_EXIT code, or a negative fault-isolation
     * code if a CPU exception killed it instead (see idt.c) -- -1 is
     * reserved above for "no such program", so a real child's exit
     * code is always distinguishable from a failed spawn. */
    return (int)exit_code;
}

int process_spawn_and_wait(const char *path, const char *thread_name) {
    return process_spawn_args_and_wait(path, thread_name, 0, NULL);
}
