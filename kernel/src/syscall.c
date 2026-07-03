#include <stdint.h>
#include "syscall.h"
#include "interrupts.h"
#include "kprintf.h"
#include "panic.h"
#include "scheduler.h"
#include "pmm.h"
#include "vmm.h"
#include "elf_loader.h"
#include "keyboard.h"
#include "process.h"
#include "vfs.h"

volatile uint64_t syscall_write_count = 0;

#define SYS_WRITE_MAX_LEN 256u /* SYS_WRITE takes a NUL-terminated string, not an (addr, len) pair,
                                 * so there's no caller-supplied length to bounds-check against --
                                 * this just bounds how far kprintf's %s is allowed to read before
                                 * finding a NUL, so a bad pointer can't run off past the end of a
                                 * genuinely valid page into who-knows-what. */

#define SYS_PATH_MAX_LEN 64u
#define SYS_EXEC_MAX_ARGS 16u
#define SYS_EXEC_MAX_ARG_LEN 128u

/* Necessary-but-not-sufficient validation: confirms the address range
 * falls inside memory this kernel actually manages (the shared
 * low-4GiB system mapping, or a process's private window), turning a
 * wild pointer into a clear panic instead of an unrelated page fault
 * three calls deep in kprintf. What it can NOT do yet is tell "this
 * process's private page" apart from "some other process's private
 * page" -- every process maps PROCESS_PRIVATE_VADDR to a *different*
 * physical page, but they all use the identical virtual address, so a
 * syscall has no way to know which one is "supposed" to be valid for
 * the caller without per-process access tracking, which doesn't exist
 * yet. Real user-pointer validation (copy_from_user-style) is a
 * further refinement once that tracking exists. */
static int user_ptr_valid(const void *ptr, uint64_t len) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    if (addr == 0 || addr + len < addr) {
        return 0;
    }
    if (addr + len <= 4ULL * 1024 * 1024 * 1024) {
        return 1;
    }
    if (addr >= PROCESS_PRIVATE_VADDR && addr + len <= PROCESS_PRIVATE_VADDR + PMM_PAGE_SIZE) {
        return 1;
    }
    /* Every program loaded via elf_load_user_program() lives somewhere
     * between its fixed load base and the top of its (fixed) stack --
     * see elf_loader.h. All loaded programs share these same two
     * constants today (there's only ever one load address), so this is
     * exactly as "necessary but not sufficient" as the check above.
     *
     * Only the *start* address needs to fit before the boundary, not
     * start+len: a NUL-terminated string (SYS_WRITE) or a buffer the
     * compiler placed within the process's own mapped stack can
     * legitimately sit close enough to ELF_USER_STACK_TOP that a
     * conservative worst-case length like SYS_WRITE_MAX_LEN would look
     * like it spills past the boundary on paper without ever actually
     * being read that far -- kprintf's %s stops at the real NUL, and a
     * compiler-placed buffer can't itself exceed the stack it was
     * allocated in. */
    return addr >= ELF_USER_LOAD_BASE && addr < ELF_USER_STACK_TOP;
}

/* Validates and bounded-copies a NUL-terminated string out of user
 * memory. Panics (rather than silently truncating) on an invalid
 * pointer -- shared by every syscall below that takes a path. */
static void copy_user_string(uint64_t user_ptr, const char *syscall_name, char *out, uint32_t out_size) {
    if (!user_ptr_valid((const void *)user_ptr, out_size)) {
        panic("syscall_dispatch: %s got an invalid pointer 0x%lx", syscall_name, user_ptr);
    }
    const char *src = (const char *)user_ptr;
    uint32_t i = 0;
    for (; i < out_size - 1 && src[i] != '\0'; i++) {
        out[i] = src[i];
    }
    out[i] = '\0';
}

/* Called from interrupt_dispatch() for vector 128 (int $0x80), reusing
 * the same interrupt_frame_t every exception/IRQ handler uses -- rax
 * is the syscall number, rdi/rsi/rdx its arguments, matching the
 * normal SysV register order. */
void syscall_dispatch(interrupt_frame_t *frame) {
    if ((frame->cs & 0x3) != 3) {
        panic("syscall_dispatch: invoked from ring %lu, expected ring 3", frame->cs & 0x3);
    }

    switch (frame->rax) {
        case SYS_WRITE:
            if (!user_ptr_valid((const void *)frame->rdi, SYS_WRITE_MAX_LEN)) {
                panic("syscall_dispatch: SYS_WRITE got an invalid pointer 0x%lx", frame->rdi);
            }
            kprintf("%s", (const char *)frame->rdi);
            syscall_write_count++;
            frame->rax = 0;
            break;

        case SYS_EXIT:
            /* A nonzero code means the caller is self-reporting a
             * failed check (see kmain.c's isolation test processes) --
             * there's still no per-process fault containment, so the
             * most honest thing to do is panic loudly rather than
             * quietly swallow a reported failure. A clean exit now
             * really ends the thread (see thread_exit()) instead of
             * yielding forever in place. */
            if ((int64_t)frame->rdi != 0) {
                panic("syscall_dispatch: user thread reported failure, exit code %ld",
                      (int64_t)frame->rdi);
            }
            kprintf("syscall: thread exited with code 0\n");
            thread_exit();
            break; /* unreachable -- thread_exit() never returns */

        case SYS_READ_CHAR: {
            int c;
            __asm__ volatile("sti");
            while ((c = keyboard_read_char()) < 0) {
                schedule();
            }
            frame->rax = (uint64_t)(int64_t)c;
            break;
        }

        case SYS_EXEC: {
            int64_t argc = (int64_t)frame->rsi;
            if (argc <= 0 || (uint64_t)argc > SYS_EXEC_MAX_ARGS) {
                panic("syscall_dispatch: SYS_EXEC got a bad argc (%ld)", argc);
            }
            if (!user_ptr_valid((const void *)frame->rdi, (uint64_t)argc * sizeof(char *))) {
                panic("syscall_dispatch: SYS_EXEC got an invalid argv array");
            }
            char **user_argv = (char **)(uintptr_t)frame->rdi;

            /* Safe as plain statics despite this call blocking (via
             * process_spawn_args_and_wait()) until the child exits,
             * even though another thread could issue its own SYS_EXEC
             * during that wait and reuse these buffers: the strings are
             * only needed long enough for elf_load_user_program() to
             * copy them into the *child's own* stack memory, which
             * happens synchronously before the wait loop below ever
             * starts, not on every loop iteration. */
            static char arg_storage[SYS_EXEC_MAX_ARGS][SYS_EXEC_MAX_ARG_LEN];
            static char *argv[SYS_EXEC_MAX_ARGS];
            for (int64_t i = 0; i < argc; i++) {
                copy_user_string((uint64_t)(uintptr_t)user_argv[i], "SYS_EXEC argv[i]",
                                  arg_storage[i], SYS_EXEC_MAX_ARG_LEN);
                argv[i] = arg_storage[i];
            }

            __asm__ volatile("sti");
            frame->rax = (uint64_t)(int64_t)process_spawn_args_and_wait(argv[0], "exec", (int)argc, argv);
            break;
        }

        case SYS_LIST_DIR: {
            char path[SYS_PATH_MAX_LEN];
            if (frame->rdi == 0) {
                path[0] = '\0';
            } else {
                copy_user_string(frame->rdi, "SYS_LIST_DIR", path, sizeof(path));
            }
            if (!user_ptr_valid((const void *)frame->rsi, frame->rdx)) {
                panic("syscall_dispatch: SYS_LIST_DIR got an invalid buffer 0x%lx (len %lu)",
                      frame->rsi, frame->rdx);
            }
            frame->rax = (uint64_t)(int64_t)vfs_list_dir(path, (char *)frame->rsi, (uint32_t)frame->rdx);
            break;
        }

        case SYS_READ_FILE: {
            char path[SYS_PATH_MAX_LEN];
            copy_user_string(frame->rdi, "SYS_READ_FILE", path, sizeof(path));
            if (!user_ptr_valid((const void *)frame->rsi, frame->rdx)) {
                panic("syscall_dispatch: SYS_READ_FILE got an invalid buffer 0x%lx (len %lu)",
                      frame->rsi, frame->rdx);
            }
            frame->rax = (uint64_t)(int64_t)vfs_read_file_into(path, (void *)frame->rsi, (uint32_t)frame->rdx);
            break;
        }

        case SYS_WRITE_FILE: {
            char path[SYS_PATH_MAX_LEN];
            copy_user_string(frame->rdi, "SYS_WRITE_FILE", path, sizeof(path));
            if (!user_ptr_valid((const void *)frame->rsi, frame->rdx)) {
                panic("syscall_dispatch: SYS_WRITE_FILE got an invalid buffer 0x%lx (len %lu)",
                      frame->rsi, frame->rdx);
            }
            int ok = vfs_write_file(path, (const void *)frame->rsi, (uint32_t)frame->rdx);
            frame->rax = ok ? 0 : (uint64_t)(int64_t)-1;
            break;
        }

        default:
            panic("syscall_dispatch: unknown syscall number %lu", frame->rax);
    }
}
