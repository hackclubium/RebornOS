#include <stddef.h>
#include <stdint.h>
#include "scheduler.h"
#include "heap.h"
#include "panic.h"
#include "vmm.h"
#include "gdt.h"
#include "interrupts.h"
#include "elf_loader.h"

typedef enum {
    THREAD_UNUSED = 0, /* free slot -- available to thread_create_ex() */
    THREAD_ACTIVE,
    THREAD_ZOMBIE, /* thread_exit() was called; schedule() reaps it after switching away */
} thread_state_t;

typedef struct {
    uint64_t rsp;
    uint64_t cr3;
    uint64_t kernel_stack_top; /* becomes TSS.RSP0 while this thread runs -- see gdt.h */
    uint8_t *stack_base;       /* the kmalloc() allocation kernel_stack_top points into, freed on exit */
    thread_state_t state;
    uint64_t heap_brk;         /* this thread's SYS_SBRK break -- see thread_get/set_heap_brk() */
    /* Where to deliver this thread's exit code, or NULL for a
     * fire-and-forget thread nobody waits on (the common case -- every
     * plain thread_create()/thread_create_process() thread). Points
     * into the *waiter's own stack frame* (e.g. a local in
     * process_spawn_and_wait()), not a slot-indexed table: a slot-
     * indexed array would need the exit code to survive from the
     * moment of exit (immediately followed by this same slot becoming
     * reusable, see schedule()'s zombie-reaping branch) until whenever
     * the waiter next gets scheduled to read it -- a window some other
     * thread_create_ex() call can and did win, silently overwriting a
     * just-exited child's real code with an unrelated thread's. A
     * pointer set atomically at creation time, before the thread is
     * ever schedulable, has no such window. */
    int64_t *exit_code_out;
    char name[16];
} thread_t;

extern void switch_context(uint64_t *old_rsp_ptr, uint64_t new_rsp);
extern void thread_trampoline(void);

static thread_t threads[SCHEDULER_MAX_THREADS];
static unsigned int active_count;
static int current_thread = -1;  /* -1 == still on the boot stack, no thread running yet */
static uint64_t discard_rsp;     /* scratch slot for a switch whose "previous" context is never resumed
                                   * (the very first switch away from the boot stack, or a thread that
                                   * just exited) */

void scheduler_init(void) {
    for (unsigned int i = 0; i < SCHEDULER_MAX_THREADS; i++) {
        threads[i].state = THREAD_UNUSED;
    }
    active_count = 0;
    current_thread = -1;
}

/* threads[]/active_count are global mutable state read and written by
 * every caller of thread_create_ex() (SYS_EXEC, process spawning, the
 * self-test threads at boot, ...) -- with the preemptive scheduler, a
 * timer tick between the free-slot scan and claiming it could let two
 * concurrent calls pick the same slot, or land active_count++ between
 * another thread's read and write of it and lose an increment (which
 * is exactly the kind of corruption that makes schedule() think the
 * wrong thread is the last one alive). irq_save_disable()/irq_restore()
 * make the whole reserve-and-initialize sequence atomic; kmalloc()
 * below already nests safely with this. */
static int thread_create_ex(const char *name, void (*entry)(void), void *arg, uint64_t cr3,
                             int64_t *exit_code_out) {
    uint64_t flags = irq_save_disable();

    int id = -1;
    for (unsigned int i = 0; i < SCHEDULER_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            id = (int)i;
            break;
        }
    }
    if (id < 0) {
        irq_restore(flags);
        panic("thread_create: too many threads (max %u)", SCHEDULER_MAX_THREADS);
    }

    uint8_t *stack = (uint8_t *)kmalloc(SCHEDULER_STACK_SIZE);
    if (stack == NULL) {
        irq_restore(flags);
        panic("thread_create: kmalloc failed for a %u-byte stack", SCHEDULER_STACK_SIZE);
    }

    /* Build a stack that looks like it already called switch_context
     * once, so the first switch into this thread "resumes" via
     * switch_context's `ret` -- into thread_trampoline, not entry()
     * directly (see context_switch.S for why: interrupts need to be
     * explicitly re-enabled here, since this path never goes through
     * iretq). Layout must match context_switch.S exactly. */
    uint64_t *sp = (uint64_t *)(stack + SCHEDULER_STACK_SIZE);
    sp[-1] = (uint64_t)(uintptr_t)thread_trampoline; /* return address `ret` pops */
    sp[-2] = 0;                                      /* rbp */
    sp[-3] = (uint64_t)(uintptr_t)entry;              /* rbx -- thread_trampoline calls *rbx */
    sp[-4] = (uint64_t)(uintptr_t)arg;                /* r12 -- thread_trampoline moves this into rdi first */
    sp[-5] = 0;                                       /* r13 */
    sp[-6] = 0;                                       /* r14 */
    sp[-7] = 0;                                       /* r15 */

    threads[id].rsp = (uint64_t)(uintptr_t)&sp[-7];
    threads[id].cr3 = cr3;
    threads[id].stack_base = stack;
    /* Reusing the same allocation as both this thread's kernel-mode
     * stack (used by thread_trampoline/entry while it's not yet in
     * ring 3) and its TSS.RSP0 landing stack (used if/when it later
     * traps back into ring 0 from ring 3) is safe: a thread is never
     * doing both at once, so nothing is ever live in both roles
     * simultaneously. */
    threads[id].kernel_stack_top = (uint64_t)(uintptr_t)(stack + SCHEDULER_STACK_SIZE);
    threads[id].state = THREAD_ACTIVE;
    threads[id].heap_brk = USER_HEAP_VADDR_START;
    threads[id].exit_code_out = exit_code_out;
    active_count++;

    unsigned int i = 0;
    for (; name[i] != '\0' && i < sizeof(threads[id].name) - 1; i++) {
        threads[id].name[i] = name[i];
    }
    threads[id].name[i] = '\0';

    irq_restore(flags);
    return id;
}

int thread_create(const char *name, void (*entry)(void)) {
    return thread_create_ex(name, entry, NULL, vmm_kernel_cr3(), NULL);
}

int thread_create_process(const char *name, void (*entry)(void), uint64_t cr3) {
    return thread_create_ex(name, entry, NULL, cr3, NULL);
}

int thread_create_process_arg(const char *name, void (*entry)(void *arg), void *arg, uint64_t cr3,
                               int64_t *exit_code_out) {
    /* Casting between void(*)(void) and void(*)(void*) is only safe
     * because thread_trampoline always calls through rbx as a bare
     * `call *%rbx` -- on the SysV x86-64 ABI an unused argument in rdi
     * is harmless, so the same internal storage works for both entry
     * shapes. */
    return thread_create_ex(name, (void (*)(void))(uintptr_t)entry, arg, cr3, exit_code_out);
}

/* Finds the next ACTIVE slot after `start`, wrapping all the way
 * around the table. With active_count > 1 (the only case schedule()
 * calls this in) this always finds something -- worst case, wrapping
 * all the way back to `start` itself if nothing else is active, though
 * that would mean active_count's bookkeeping is wrong somewhere. */
static int next_active_after(int start) {
    for (unsigned int i = 1; i <= SCHEDULER_MAX_THREADS; i++) {
        int idx = (start + (int)i) % (int)SCHEDULER_MAX_THREADS;
        if (idx < 0) {
            idx += (int)SCHEDULER_MAX_THREADS;
        }
        if (threads[idx].state == THREAD_ACTIVE) {
            return idx;
        }
    }
    return -1;
}

void schedule(void) {
    /* This whole function must run atomically. In particular, a timer
     * IRQ landing between kfree(prev's stack) and switch_context()
     * below would recursively re-enter schedule() *on top of that same
     * stack* (an interrupt doesn't switch stacks within the same ring),
     * see the zombie-reaping branch further down for the corrupted-rsp
     * consequences of getting this wrong. Restored implicitly whenever
     * whichever thread runs next takes its own interrupt/syscall round
     * trip back out through iretq -- same as every other place in this
     * kernel that leaves IF=0 across a context switch. */
    __asm__ volatile("cli");

    if (active_count <= 1) {
        return;
    }

    int prev = current_thread;
    int next = next_active_after(prev);
    if (next < 0) {
        return; /* shouldn't happen given active_count > 1, but don't switch into nothing */
    }
    current_thread = next;

    vmm_load_cr3(threads[next].cr3);
    tss_set_kernel_stack(threads[next].kernel_stack_top);

    if (prev == -1) {
        switch_context(&discard_rsp, threads[next].rsp);
        return;
    }

    if (threads[prev].state == THREAD_ZOMBIE) {
        /* prev is exiting and will never be resumed. Freeing its stack
         * here, before switch_context, is safe even though we're still
         * physically executing on that stack's memory: freeing just
         * marks the heap block free in the free-list, it doesn't
         * invalidate the underlying physical page, and nothing else
         * can touch that block before switch_context changes rsp on
         * the very next line, since we hold the CPU the whole time. */
        kfree(threads[prev].stack_base);
        threads[prev].state = THREAD_UNUSED;
        active_count--;
        switch_context(&discard_rsp, threads[next].rsp);
        return;
    }

    switch_context(&threads[prev].rsp, threads[next].rsp);
}

__attribute__((noreturn)) void thread_exit_code(int64_t code) {
    if (current_thread < 0) {
        panic("thread_exit_code: called with no current thread");
    }
    if (threads[current_thread].exit_code_out != NULL) {
        *threads[current_thread].exit_code_out = code;
    }
    threads[current_thread].state = THREAD_ZOMBIE;
    schedule();
    panic("thread_exit_code: schedule() returned into an exited thread");
}

__attribute__((noreturn)) void thread_exit(void) {
    thread_exit_code(0);
}

int thread_is_alive(int tid) {
    if (tid < 0 || tid >= (int)SCHEDULER_MAX_THREADS) {
        return 0;
    }
    return threads[tid].state == THREAD_ACTIVE;
}

uint64_t thread_get_heap_brk(void) {
    return threads[current_thread].heap_brk;
}

void thread_set_heap_brk(uint64_t brk) {
    threads[current_thread].heap_brk = brk;
}

void scheduler_start(void) {
    __asm__ volatile("cli"); /* same reasoning as schedule()'s -- keep the switch atomic */
    if (active_count == 0) {
        panic("scheduler_start: no threads created");
    }
    int first = next_active_after(-1);
    current_thread = first;
    vmm_load_cr3(threads[first].cr3);
    tss_set_kernel_stack(threads[first].kernel_stack_top);
    switch_context(&discard_rsp, threads[first].rsp);
    panic("scheduler_start: switch_context returned to the boot stack unexpectedly");
}
