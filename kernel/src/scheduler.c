#include <stddef.h>
#include <stdint.h>
#include "scheduler.h"
#include "heap.h"
#include "panic.h"

typedef struct {
    uint64_t rsp;
    char name[16];
} thread_t;

extern void switch_context(uint64_t *old_rsp_ptr, uint64_t new_rsp);
extern void thread_trampoline(void);

static thread_t threads[SCHEDULER_MAX_THREADS];
static unsigned int thread_count;
static int current_thread = -1;   /* -1 == still on the boot stack, no thread running yet */
static uint64_t boot_rsp_unused;  /* discard slot for the very first switch away from boot */

void scheduler_init(void) {
    thread_count = 0;
    current_thread = -1;
}

int thread_create(const char *name, void (*entry)(void)) {
    if (thread_count >= SCHEDULER_MAX_THREADS) {
        panic("thread_create: too many threads (max %u)", SCHEDULER_MAX_THREADS);
    }

    uint8_t *stack = (uint8_t *)kmalloc(SCHEDULER_STACK_SIZE);
    if (stack == NULL) {
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
    sp[-4] = 0;                                       /* r12 */
    sp[-5] = 0;                                       /* r13 */
    sp[-6] = 0;                                       /* r14 */
    sp[-7] = 0;                                       /* r15 */

    unsigned int id = thread_count++;
    threads[id].rsp = (uint64_t)(uintptr_t)&sp[-7];

    unsigned int i = 0;
    for (; name[i] != '\0' && i < sizeof(threads[id].name) - 1; i++) {
        threads[id].name[i] = name[i];
    }
    threads[id].name[i] = '\0';

    return (int)id;
}

void schedule(void) {
    if (thread_count <= 1) {
        return;
    }

    int prev = current_thread;
    int next = (prev + 1) % (int)thread_count;
    current_thread = next;

    uint64_t *old_rsp_slot = (prev == -1) ? &boot_rsp_unused : &threads[prev].rsp;
    switch_context(old_rsp_slot, threads[next].rsp);
}

void scheduler_start(void) {
    if (thread_count == 0) {
        panic("scheduler_start: no threads created");
    }
    current_thread = 0;
    switch_context(&boot_rsp_unused, threads[0].rsp);
    panic("scheduler_start: switch_context returned to the boot stack unexpectedly");
}
