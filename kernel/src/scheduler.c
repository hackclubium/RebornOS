#include <stddef.h>
#include <stdint.h>
#include "scheduler.h"
#include "heap.h"
#include "panic.h"
#include "vmm.h"
#include "gdt.h"
#include "interrupts.h"
#include "elf_loader.h"
#include "spinlock.h"
#include "smp.h"

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
    /* Which core is currently executing this thread, or -1 if it's
     * ACTIVE but not presently running anywhere (i.e. genuinely ready
     * to be picked up by whichever core's schedule() looks next). A
     * single shared ready queue with real cross-core scheduling means
     * two cores could otherwise both pick the same THREAD_ACTIVE slot
     * out of threads[] at once and both try to run it -- this is what
     * next_ready_after() actually checks, on top of state. Threads
     * aren't pinned: whichever core's schedule() gets to a ready thread
     * first runs it, so the same logical thread can migrate from core
     * to core across separate scheduling quantums. volatile: cleared
     * from inside switch_context_release() (see context_switch.S),
     * not from plain C, so the compiler must not cache a stale read
     * across that call. */
    volatile int running_on_core;
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
    /* True only for the one dedicated idle thread each core owns (see
     * scheduler_create_idle_threads()) -- excluded from next_ready_after()'s
     * normal scan so no other core can ever pick it up; a core only
     * ever runs its own. Exists so a core whose own thread just exited,
     * with genuinely nothing else ready for it, always has *something*
     * valid to switch to instead of needing to spin waiting for a
     * thread that might never appear (the previous approach: retrying
     * forever livelocked whenever the only remaining threads were all
     * already running, uninterrupted, on other cores). */
    int is_idle;
    char name[16];
} thread_t;

extern void switch_context(uint64_t *old_rsp_ptr, uint64_t new_rsp);
extern void switch_context_release(uint64_t *old_rsp_ptr, uint64_t new_rsp, volatile int *release_flag);
extern void thread_trampoline(void);

/* threads[]/active_count/each thread's state and running_on_core are
 * shared, global mutable state read and written from every core now
 * that scheduled threads genuinely run on more than one at once (not
 * just interleaved on a single core the way preemption alone allows) --
 * scheduler_lock covers all of it. irq_save_disable()/irq_restore()
 * still run alongside it everywhere: the lock alone stops another
 * *core* from touching this state concurrently, but does nothing to
 * stop this *same* core's own timer/LAPIC-timer interrupt recursively
 * calling back into schedule() while the lock is already held by this
 * core, which would spin forever waiting for a lock it itself holds. */
static spinlock_t scheduler_lock = SPINLOCK_INIT;

static thread_t threads[SCHEDULER_MAX_THREADS];
static unsigned int active_count;
/* Per-core "what am I running right now" -- -1 means this core hasn't
 * picked up a thread yet (still on its own boot/AP-entry stack). */
static int current_thread[SMP_MAX_CPUS];
/* Per-core scratch slot for a switch_context() whose "previous" context
 * is never resumed (this core's very first switch away from its own
 * boot stack, or away from a thread that just became a zombie). One
 * per core so two cores hitting either case at the same moment don't
 * write through the same pointer. */
static uint64_t discard_rsp[SMP_MAX_CPUS];
/* Each core's dedicated idle thread id, set once by
 * scheduler_create_idle_threads(); -1 until then. */
static int idle_tid[SMP_MAX_CPUS];

static void idle_loop(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void scheduler_init(void) {
    for (unsigned int i = 0; i < SCHEDULER_MAX_THREADS; i++) {
        threads[i].state = THREAD_UNUSED;
        threads[i].running_on_core = -1;
    }
    active_count = 0;
    for (unsigned int i = 0; i < SMP_MAX_CPUS; i++) {
        current_thread[i] = -1;
        idle_tid[i] = -1;
    }
}

static int thread_create_ex(const char *name, void (*entry)(void), void *arg, uint64_t cr3,
                             int64_t *exit_code_out) {
    uint64_t flags = irq_save_disable();
    spinlock_acquire(&scheduler_lock);

    int id = -1;
    for (unsigned int i = 0; i < SCHEDULER_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_UNUSED) {
            id = (int)i;
            break;
        }
    }
    if (id < 0) {
        spinlock_release(&scheduler_lock);
        irq_restore(flags);
        panic("thread_create: too many threads (max %u)", SCHEDULER_MAX_THREADS);
    }

    uint8_t *stack = (uint8_t *)kmalloc(SCHEDULER_STACK_SIZE);
    if (stack == NULL) {
        spinlock_release(&scheduler_lock);
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
    threads[id].running_on_core = -1; /* ready, not yet running anywhere */
    threads[id].heap_brk = USER_HEAP_VADDR_START;
    threads[id].exit_code_out = exit_code_out;
    threads[id].is_idle = 0;
    active_count++;

    unsigned int i = 0;
    for (; name[i] != '\0' && i < sizeof(threads[id].name) - 1; i++) {
        threads[id].name[i] = name[i];
    }
    threads[id].name[i] = '\0';

    spinlock_release(&scheduler_lock);
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

/* Finds the next slot after `start` that's both ACTIVE and not already
 * running on some other core, wrapping all the way around the table --
 * must be called with scheduler_lock already held. With active_count
 * covering at least one genuinely ready thread (the only case schedule()
 * calls this in) this always finds something. */
static int next_ready_after(int start) {
    for (unsigned int i = 1; i <= SCHEDULER_MAX_THREADS; i++) {
        int idx = (start + (int)i) % (int)SCHEDULER_MAX_THREADS;
        if (idx < 0) {
            idx += (int)SCHEDULER_MAX_THREADS;
        }
        if (threads[idx].state == THREAD_ACTIVE && threads[idx].running_on_core < 0 && !threads[idx].is_idle) {
            return idx;
        }
    }
    return -1;
}

/* Creates one dedicated, never-exiting idle thread per core (indices
 * 0..cpu_count-1) and records each in idle_tid[] -- called once from
 * kmain(), after scheduler_init() and before any core can possibly
 * need to fall back to one. Ordinary thread_create_ex() bookkeeping
 * (its own stack, its own slot, counted in active_count) applies
 * normally; what makes it special is purely the is_idle flag, which
 * next_ready_after() checks to keep it invisible to every core except
 * the one that falls back to it explicitly (see schedule()/
 * scheduler_start()). */
void scheduler_create_idle_threads(uint32_t cpu_count) {
    for (uint32_t core = 0; core < cpu_count; core++) {
        int tid = thread_create_ex("idle", idle_loop, NULL, vmm_kernel_cr3(), NULL);
        threads[tid].is_idle = 1;
        idle_tid[core] = tid;
    }
}

void schedule(void) {
    /* This whole function must run atomically with respect to this
     * core's own interrupts. In particular, a timer IRQ landing between
     * kfree(prev's stack) and switch_context() below would recursively
     * re-enter schedule() *on top of that same stack* (an interrupt
     * doesn't switch stacks within the same ring), see the
     * zombie-reaping branch further down for the corrupted-rsp
     * consequences of getting this wrong. Restored implicitly whenever
     * whichever thread runs next takes its own interrupt/syscall round
     * trip back out through iretq -- same as every other place in this
     * kernel that leaves IF=0 across a context switch. */
    __asm__ volatile("cli");
    uint32_t core = smp_current_cpu_index();

    spinlock_acquire(&scheduler_lock);

    int prev = current_thread[core];
    /* A zombie prev can never be resumed (thread_exit_code() is
     * waiting for this call to switch away and never return) -- unlike
     * an ordinary preemption, "nothing else ready right now" isn't a
     * valid reason to give up here. With real cross-core scheduling,
     * active_count > 1 no longer guarantees next_ready_after() finds
     * something: every *other* active thread might genuinely be
     * running, uninterrupted, on one of the other cores -- that's what
     * this core's own idle thread (see scheduler_create_idle_threads())
     * exists to fall back to instead of assuming something will free up. */
    int prev_is_zombie = (prev >= 0 && threads[prev].state == THREAD_ZOMBIE);

    if (!prev_is_zombie && active_count <= 1) {
        spinlock_release(&scheduler_lock);
        return;
    }

    int next = next_ready_after(prev);
    if (next < 0) {
        if (!prev_is_zombie) {
            spinlock_release(&scheduler_lock);
            return; /* nothing else ready for this core right now -- keep running prev */
        }
        /* prev is a zombie and nothing else is ready -- fall back to
         * this core's own idle thread, guaranteed to always be
         * available (see scheduler_create_idle_threads()), rather than
         * assuming something will eventually free up (it might not:
         * every other active thread could just keep running,
         * uninterrupted, on its own core forever). */
        next = idle_tid[core];
    }
    current_thread[core] = next;
    threads[next].running_on_core = (int)core;

    /* Freeing prev's stack (if it's a zombie) has to happen before we
     * release the lock and switch away, but the kfree() call itself
     * must NOT happen while still holding scheduler_lock: kfree() takes
     * heap_lock, and holding two locks across a call invites the
     * classic lock-ordering deadlock if anything elsewhere ever takes
     * them in the opposite order. Decide what to do here, release
     * scheduler_lock, then act.
     *
     * Deliberately NOT clearing a still-alive prev's running_on_core
     * here, unlike the zombie case: this thread's threads[prev].rsp
     * hasn't been written yet (switch_context() below is what writes
     * it) -- clearing the flag now, before that store, would let
     * another core see prev as stealable and resume it from a stale
     * rsp while this core is still mid-save. switch_context_release()
     * clears it atomically in the right order instead (see
     * context_switch.S). A zombie's rsp is never read by anyone again
     * (its slot goes to THREAD_UNUSED, which next_ready_after() already
     * excludes regardless of running_on_core), so no such ordering
     * constraint applies there. */
    uint8_t *zombie_stack = NULL;
    if (prev_is_zombie) {
        zombie_stack = threads[prev].stack_base;
        threads[prev].state = THREAD_UNUSED;
        threads[prev].running_on_core = -1;
        active_count--;
    }

    vmm_load_cr3(threads[next].cr3);
    tss_set_kernel_stack(core, threads[next].kernel_stack_top);

    spinlock_release(&scheduler_lock);

    if (prev < 0) {
        switch_context(&discard_rsp[core], threads[next].rsp);
        return;
    }

    if (zombie_stack != NULL) {
        /* Freeing prev's stack here, before switch_context, is safe
         * even though we're still physically executing on that stack's
         * memory: freeing just marks the heap block free in the
         * free-list, it doesn't invalidate the underlying physical
         * page, and nothing else can touch that block before
         * switch_context changes rsp on the very next line, since this
         * core holds the CPU the whole time. */
        kfree(zombie_stack);
        switch_context(&discard_rsp[core], threads[next].rsp);
        return;
    }

    switch_context_release(&threads[prev].rsp, threads[next].rsp, &threads[prev].running_on_core);
}

__attribute__((noreturn)) void thread_exit_code(int64_t code) {
    uint32_t core = smp_current_cpu_index();
    int tid = current_thread[core];
    if (tid < 0) {
        panic("thread_exit_code: called with no current thread");
    }
    if (threads[tid].exit_code_out != NULL) {
        *threads[tid].exit_code_out = code;
    }

    /* Deliberately no matching irq_restore(): schedule() below always
     * either switches away for good (the common case, since prev is
     * now a zombie -- see schedule()'s comments) or, if it doesn't,
     * hits the panic right after anyway. Restoring interrupts here and
     * then calling schedule() (which re-disables them as its own
     * first instruction) leaves a brief real window where a timer/
     * LAPIC-timer interrupt could land on this exact core between the
     * two -- a nested, nothing-to-do-with-us nested schedule() call
     * right in the middle of this exact sequence, immediately after
     * this thread became a zombie but before this call ever reaches
     * its own schedule(). Keeping interrupts off continuously from
     * here through schedule()'s own decision closes that window. */
    irq_save_disable();
    spinlock_acquire(&scheduler_lock);
    threads[tid].state = THREAD_ZOMBIE;
    spinlock_release(&scheduler_lock);

    /* schedule() should never return here: with prev a zombie, it
     * always either finds a real ready thread or falls back to this
     * core's own idle thread (see scheduler_create_idle_threads()) --
     * there is structurally nothing else for it to do but switch away.
     * If it ever does return anyway, retrying costs nothing (this
     * thread has no valid state left to preserve or corrupt further)
     * and is safer than taking the whole system down over what's, at
     * worst, a transient condition on some other core resolving a
     * moment later. */
    for (;;) {
        schedule();
    }
}

__attribute__((noreturn)) void thread_exit(void) {
    thread_exit_code(0);
}

/* Reached only from thread_trampoline (context_switch.S) when the entry
 * pointer it just popped into rbx reads back as NULL, which should be
 * structurally impossible -- thread_create_ex() always writes a real
 * function pointer into that stack slot before the thread is ever
 * schedulable, and nothing else should be writing to a not-yet-run
 * thread's stack. Exists purely to turn an otherwise opaque "invalid
 * opcode at rip=0x0" crash into a full dump of this thread's recorded
 * state and its stack's actual current contents, to catch whatever
 * clobbered that slot between construction and this thread's first run. */
__attribute__((noreturn)) void thread_trampoline_null_entry_panic(void) {
    uint32_t core = smp_current_cpu_index();
    int tid = current_thread[core];
    if (tid < 0) {
        panic("thread_trampoline: entry (rbx) is NULL and current_thread[core %u] is -1", core);
    }
    uint64_t top = threads[tid].kernel_stack_top;
    uint64_t *slot = (uint64_t *)(uintptr_t)top;
    panic("thread_trampoline: entry (rbx) is NULL for tid=%d name=\"%s\" core=%u state=%d "
          "running_on_core=%d active_count=%u rsp=0x%lx stack_base=0x%lx kernel_stack_top=0x%lx "
          "top-of-stack words [-1..-7]=0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx",
          tid, threads[tid].name, core, (int)threads[tid].state, threads[tid].running_on_core,
          active_count, threads[tid].rsp, (uint64_t)(uintptr_t)threads[tid].stack_base, top,
          slot[-1], slot[-2], slot[-3], slot[-4], slot[-5], slot[-6], slot[-7]);
}

int thread_is_alive(int tid) {
    if (tid < 0 || tid >= (int)SCHEDULER_MAX_THREADS) {
        return 0;
    }
    return threads[tid].state == THREAD_ACTIVE;
}

/* Diagnostic accessor: which thread (name, or "(none)" before this core
 * has picked one up yet) is current_thread[] for the calling core right
 * now. Used by exception_handler() (idt.c) to identify which thread and
 * core a fault happened on/in -- rip alone doesn't say that. */
const char *thread_current_name(void) {
    int tid = current_thread[smp_current_cpu_index()];
    if (tid < 0) {
        return "(none)";
    }
    return threads[tid].name;
}

uint64_t thread_get_heap_brk(void) {
    return threads[current_thread[smp_current_cpu_index()]].heap_brk;
}

void thread_set_heap_brk(uint64_t brk) {
    threads[current_thread[smp_current_cpu_index()]].heap_brk = brk;
}

/* Picks a ready thread for *this* core and switches into it, never
 * returning -- called once by the BSP after all the boot-time threads
 * are created, and once by every AP after it's finished its own
 * per-core setup (see ap_entry() in smp.c). Both cases are the exact
 * same operation: "this core has no thread yet, go get one from the
 * shared ready queue" -- the BSP just happens to be first. */
void scheduler_start(void) {
    __asm__ volatile("cli"); /* same reasoning as schedule()'s -- keep the switch atomic */
    uint32_t core = smp_current_cpu_index();

    spinlock_acquire(&scheduler_lock);
    if (active_count == 0) {
        spinlock_release(&scheduler_lock);
        panic("scheduler_start: no threads created");
    }
    int first = next_ready_after(-1);
    if (first < 0) {
        /* Every existing thread happens to be claimed by some other
         * core at this exact instant (see schedule()'s matching
         * comment) -- fall back to this core's own idle thread,
         * guaranteed to be free (see scheduler_create_idle_threads()),
         * rather than assuming active_count > 0 means an immediate
         * real pick is always available. */
        first = idle_tid[core];
    }
    current_thread[core] = first;
    threads[first].running_on_core = (int)core;
    vmm_load_cr3(threads[first].cr3);
    tss_set_kernel_stack(core, threads[first].kernel_stack_top);
    spinlock_release(&scheduler_lock);

    switch_context(&discard_rsp[core], threads[first].rsp);
    panic("scheduler_start: switch_context returned to the boot stack unexpectedly");
}
