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
#include "mouse.h"
#include "framebuffer.h"
#include "process.h"
#include "vfs.h"
#include "minilib.h"

volatile uint64_t syscall_write_count = 0;

#define SYS_WRITE_MAX_LEN 256u /* SYS_WRITE takes a NUL-terminated string, not an (addr, len)
                                 * pair -- this just caps how much a single call can print. */
#define SYS_PATH_MAX_LEN 64u
#define SYS_EXEC_MAX_ARGS 16u
#define SYS_EXEC_MAX_ARG_LEN 128u

/* Real per-process pointer validation: walks the caller's own page
 * tables (vmm_check_range(), given the CR3 a syscall always runs
 * under -- entering ring 0 via int $0x80 never switches address
 * spaces) to confirm every page in [ptr, ptr+len) is actually mapped,
 * user-accessible, and (if need_write) writable. Replaces the old
 * coarse "is this address in one of a few known ranges" heuristic --
 * this works for any buffer the process legitimately has mapped, not
 * just a handful of hardcoded regions. */
static int user_ptr_valid(const void *ptr, uint64_t len, int need_write) {
    return vmm_check_range(vmm_current_cr3(), (uint64_t)(uintptr_t)ptr, len, need_write);
}

/* Validates and bounded-copies a NUL-terminated string out of user
 * memory, one page at a time -- re-checking with vmm_check_range()
 * whenever the read crosses into a new page, rather than requiring the
 * whole out_size worst case to be pre-validated up front (a short
 * string legitimately placed right at the edge of mapped memory, e.g.
 * near the top of the stack, would otherwise look like it "might" spill
 * past the boundary on paper even though the real read never reaches
 * that far). Panics on an invalid pointer -- shared by every syscall
 * below that takes a path or string argument. */
static void copy_user_string(uint64_t user_ptr, const char *syscall_name, char *out, uint32_t out_size) {
    uint64_t pml4 = vmm_current_cr3();
    uint64_t addr = user_ptr;
    uint32_t i = 0;
    for (; i < out_size - 1; i++, addr++) {
        if (addr % PMM_PAGE_SIZE == 0 || i == 0) {
            if (!vmm_check_range(pml4, addr, 1, 0)) {
                panic("syscall_dispatch: %s got an invalid pointer 0x%lx", syscall_name, user_ptr);
            }
        }
        char c = *(const char *)(uintptr_t)addr;
        if (c == '\0') {
            break;
        }
        out[i] = c;
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
        case SYS_WRITE: {
            char msg[SYS_WRITE_MAX_LEN];
            copy_user_string(frame->rdi, "SYS_WRITE", msg, sizeof(msg));
            kprintf("%s", msg);
            syscall_write_count++;
            frame->rax = 0;
            break;
        }

        case SYS_EXIT:
            /* A nonzero code means the caller is self-reporting a
             * failed check (see kmain.c's isolation test processes) --
             * now that real per-process fault containment exists (see
             * thread_exit_code() in scheduler.c and the CPU exception
             * handler in idt.c), the code is simply handed
             * back to whoever's waiting (process_spawn_and_wait())
             * instead of panicking the whole kernel over it. */
            kprintf("syscall: thread exited with code %ld\n", (int64_t)frame->rdi);
            thread_exit_code((int64_t)frame->rdi);
            break; /* unreachable -- thread_exit_code() never returns */

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
            if (!user_ptr_valid((const void *)frame->rdi, (uint64_t)argc * sizeof(char *), 0)) {
                panic("syscall_dispatch: SYS_EXEC got an invalid argv array");
            }
            char **user_argv = (char **)(uintptr_t)frame->rdi;

            /* Plain (non-static) locals, each on this syscall's own
             * caller's kernel stack: with real cross-core scheduling,
             * two threads on two different cores can now genuinely
             * execute SYS_EXEC's argument-copying step at the exact
             * same instant, which a single shared static buffer can't
             * survive (unlike the single-core-only case this used to
             * be, where only one thread's syscall handler ever actually
             * executed at a time). ~2.2KB combined, comfortably inside
             * SCHEDULER_STACK_SIZE's 16KB per-thread kernel stack. */
            char arg_storage[SYS_EXEC_MAX_ARGS][SYS_EXEC_MAX_ARG_LEN];
            char *argv[SYS_EXEC_MAX_ARGS];
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
            if (!user_ptr_valid((const void *)frame->rsi, frame->rdx, 1)) {
                panic("syscall_dispatch: SYS_LIST_DIR got an invalid buffer 0x%lx (len %lu)",
                      frame->rsi, frame->rdx);
            }
            frame->rax = (uint64_t)(int64_t)vfs_list_dir(path, (char *)frame->rsi, (uint32_t)frame->rdx);
            break;
        }

        case SYS_READ_FILE: {
            char path[SYS_PATH_MAX_LEN];
            copy_user_string(frame->rdi, "SYS_READ_FILE", path, sizeof(path));
            if (!user_ptr_valid((const void *)frame->rsi, frame->rdx, 1)) {
                panic("syscall_dispatch: SYS_READ_FILE got an invalid buffer 0x%lx (len %lu)",
                      frame->rsi, frame->rdx);
            }
            frame->rax = (uint64_t)(int64_t)vfs_read_file_into(path, (void *)frame->rsi, (uint32_t)frame->rdx);
            break;
        }

        case SYS_WRITE_FILE: {
            char path[SYS_PATH_MAX_LEN];
            copy_user_string(frame->rdi, "SYS_WRITE_FILE", path, sizeof(path));
            if (!user_ptr_valid((const void *)frame->rsi, frame->rdx, 0)) {
                panic("syscall_dispatch: SYS_WRITE_FILE got an invalid buffer 0x%lx (len %lu)",
                      frame->rsi, frame->rdx);
            }
            int ok = vfs_write_file(path, (const void *)frame->rsi, (uint32_t)frame->rdx);
            frame->rax = ok ? 0 : (uint64_t)(int64_t)-1;
            break;
        }

        case SYS_FB_INFO: {
            struct __attribute__((packed)) {
                uint32_t width, height, pixels_per_scanline, bytes_per_pixel;
                uint8_t red_shift, green_shift, blue_shift, _pad;
            } out;
            if (!user_ptr_valid((const void *)frame->rdi, sizeof(out), 1)) {
                panic("syscall_dispatch: SYS_FB_INFO got an invalid buffer 0x%lx", frame->rdi);
            }
            const boot_framebuffer_t *fb = fb_get_info();
            out.width = fb->width;
            out.height = fb->height;
            out.pixels_per_scanline = fb->pixels_per_scanline;
            out.bytes_per_pixel = fb->bytes_per_pixel;
            out.red_shift = fb->red_shift;
            out.green_shift = fb->green_shift;
            out.blue_shift = fb->blue_shift;
            out._pad = 0;
            memcpy((void *)(uintptr_t)frame->rdi, &out, sizeof(out));
            frame->rax = 0;
            break;
        }

        case SYS_FB_MAP: {
            const boot_framebuffer_t *fb = fb_get_info();
            uint64_t pml4 = vmm_current_cr3();
            uint64_t page_count = (fb->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
            for (uint64_t i = 0; i < page_count; i++) {
                uint64_t vaddr = USER_FB_VADDR + i * PMM_PAGE_SIZE;
                if (!vmm_page_present(pml4, vaddr)) {
                    vmm_map_page(pml4, vaddr, fb->base + i * PMM_PAGE_SIZE, VMM_MAP_WRITE);
                }
            }
            frame->rax = USER_FB_VADDR;
            break;
        }

        case SYS_MOUSE_READ: {
            struct __attribute__((packed)) {
                int32_t x, y;
                uint8_t buttons;
                uint8_t _pad[3];
            } out;
            if (!user_ptr_valid((const void *)frame->rdi, sizeof(out), 1)) {
                panic("syscall_dispatch: SYS_MOUSE_READ got an invalid buffer 0x%lx", frame->rdi);
            }
            mouse_state_t state;
            mouse_get_state(&state);
            out.x = state.x;
            out.y = state.y;
            out.buttons = state.buttons;
            out._pad[0] = out._pad[1] = out._pad[2] = 0;
            memcpy((void *)(uintptr_t)frame->rdi, &out, sizeof(out));
            frame->rax = 0;
            break;
        }

        case SYS_SBRK: {
            int64_t increment = (int64_t)frame->rdi;
            if (increment < 0) {
                frame->rax = (uint64_t)(int64_t)-1; /* shrinking the heap isn't supported yet */
                break;
            }
            uint64_t old_brk = thread_get_heap_brk();
            uint64_t new_brk = old_brk + (uint64_t)increment;
            uint64_t pml4 = vmm_current_cr3();
            uint64_t start_page = old_brk & ~(uint64_t)(PMM_PAGE_SIZE - 1);
            uint64_t end_page = (new_brk + PMM_PAGE_SIZE - 1) & ~(uint64_t)(PMM_PAGE_SIZE - 1);
            int ok = 1;
            for (uint64_t p = start_page; p < end_page; p += PMM_PAGE_SIZE) {
                if (vmm_page_present(pml4, p)) {
                    continue; /* sbrk(0), or growth that stays within an already-mapped page */
                }
                void *phys = pmm_alloc_page();
                if (phys == NULL) {
                    ok = 0;
                    break;
                }
                memset(phys, 0, PMM_PAGE_SIZE);
                vmm_map_page(pml4, p, (uint64_t)(uintptr_t)phys, VMM_MAP_WRITE);
            }
            if (ok) {
                thread_set_heap_brk(new_brk);
                frame->rax = old_brk;
            } else {
                frame->rax = (uint64_t)(int64_t)-1;
            }
            break;
        }

        default:
            panic("syscall_dispatch: unknown syscall number %lu", frame->rax);
    }
}
