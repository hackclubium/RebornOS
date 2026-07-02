#include <stdarg.h>
#include <stdint.h>
#include "panic.h"
#include "kprintf.h"
#include "framebuffer.h"
#include "qemu_debug.h"

/* Requires the kernel to be built with -fno-omit-frame-pointer so %rbp
 * always heads a valid [saved rbp][return addr] frame we can walk. Raw
 * addresses only -- resolving them to function names/lines is done
 * offline with tools/symbolize.sh against kernel.elf's debug info,
 * rather than carrying a symbol table in the freestanding kernel. */
static inline uint64_t read_rbp(void) {
    uint64_t v;
    __asm__ volatile("mov %%rbp, %0" : "=r"(v));
    return v;
}

static void print_stack_trace(void) {
    uint64_t rbp = read_rbp();
    kprintf("stack trace (resolve with tools/symbolize.sh):\n");
    for (int i = 0; i < 16 && rbp; i++) {
        uint64_t *frame = (uint64_t *)(uintptr_t)rbp;
        uint64_t return_addr = frame[1];
        if (!return_addr) {
            break;
        }
        kprintf("  #%d 0x%lx\n", i, return_addr);
        rbp = frame[0];
    }
}

/* Guards against panic() recursing into itself: a fault triggered by
 * panic()'s own printing (e.g. print_stack_trace() walking into a
 * garbage %rbp -- rbp is not reloaded on a ring3->ring0 transition, so
 * it can hold whatever a user program last left there) would otherwise
 * re-enter panic() from inside exception_handler(), which tries the
 * exact same unsafe printing again, faults again, and so on -- an
 * unbounded fault storm instead of a clean exit. The second entry
 * skips straight to qemu_debug_exit() instead of repeating the attempt. */
static volatile int panicking = 0;

void panic(const char *fmt, ...) {
    __asm__ volatile("cli");

    if (panicking) {
        qemu_debug_exit(QEMU_DEBUG_EXIT_FAILURE);
    }
    panicking = 1;

    kprintf("\n*** KERNEL PANIC ***\n");
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
    kprintf("\n");

    print_stack_trace();

    fb_clear(0x8B0000);
    fb_puts(8, 8, "KERNEL PANIC - see serial log for details", 0xFFFFFF, 0x8B0000);

    qemu_debug_exit(QEMU_DEBUG_EXIT_FAILURE);
}
