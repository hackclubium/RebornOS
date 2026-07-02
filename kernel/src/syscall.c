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
     * exactly as "necessary but not sufficient" as the check above. */
    return addr >= ELF_USER_LOAD_BASE && addr + len <= ELF_USER_STACK_TOP;
}

/* Called from interrupt_dispatch() for vector 128 (int $0x80), reusing
 * the same interrupt_frame_t every exception/IRQ handler uses -- rax
 * is the syscall number, rdi its first argument, matching the normal
 * SysV register order. */
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
            if (!user_ptr_valid((const void *)frame->rdi, 64)) {
                panic("syscall_dispatch: SYS_EXEC got an invalid pointer 0x%lx", frame->rdi);
            }
            const char *user_name = (const char *)frame->rdi;
            char name[64];
            uint32_t i = 0;
            for (; i < sizeof(name) - 1 && user_name[i] != '\0'; i++) {
                name[i] = user_name[i];
            }
            name[i] = '\0';

            __asm__ volatile("sti");
            frame->rax = (uint64_t)(int64_t)process_spawn_and_wait(name, "exec");
            break;
        }

        case SYS_LIST_ROOT: {
            if (!user_ptr_valid((const void *)frame->rdi, frame->rsi)) {
                panic("syscall_dispatch: SYS_LIST_ROOT got an invalid buffer 0x%lx (len %lu)",
                      frame->rdi, frame->rsi);
            }
            frame->rax = vfs_list_root((char *)frame->rdi, (uint32_t)frame->rsi);
            break;
        }

        default:
            panic("syscall_dispatch: unknown syscall number %lu", frame->rax);
    }
}
