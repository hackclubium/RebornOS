#include "syscall.h"
#include "interrupts.h"
#include "kprintf.h"
#include "panic.h"
#include "scheduler.h"

volatile uint64_t syscall_write_count = 0;

/* Called from interrupt_dispatch() for vector 128 (int $0x80), reusing
 * the same interrupt_frame_t every exception/IRQ handler uses -- rax
 * is the syscall number, rdi its first argument, matching the normal
 * SysV register order.
 *
 * There is no per-process address space yet (see gdt.h/vmm.c), so
 * every pointer a syscall receives from "user" code is, for now,
 * genuinely valid kernel-accessible memory -- trusting frame->rdi
 * directly below isn't a bug today, but it would become one the
 * moment a real isolated user address space exists, at which point
 * every user pointer needs validation (copy_from_user-style) before
 * the kernel dereferences it. */
void syscall_dispatch(interrupt_frame_t *frame) {
    if ((frame->cs & 0x3) != 3) {
        panic("syscall_dispatch: invoked from ring %lu, expected ring 3", frame->cs & 0x3);
    }

    switch (frame->rax) {
        case SYS_WRITE:
            kprintf("%s", (const char *)frame->rdi);
            syscall_write_count++;
            frame->rax = 0;
            break;

        case SYS_EXIT:
            /* We're inside an interrupt gate (IF=0), so looping on hlt
             * here would freeze the entire system forever, not just
             * this thread -- nothing could ever interrupt us again.
             * Our minimal scheduler has no way to remove a thread from
             * the round-robin, so instead this thread "exits" by
             * voluntarily yielding every time it gets a turn, forever.
             * A real thread-exit would deallocate its stack and remove
             * it from the ready queue entirely. */
            kprintf("syscall: user thread exited with code %ld\n", (int64_t)frame->rdi);
            for (;;) {
                schedule();
            }
            break;

        default:
            panic("syscall_dispatch: unknown syscall number %lu", frame->rax);
    }
}
