#ifndef REBORNOS_SCHEDULER_H
#define REBORNOS_SCHEDULER_H

#include <stdint.h>

/* Generous headroom over the boot-time self-test roster: several test
 * threads (exec-wait, subdir, argv, stack, network, smp, gfx-test) each
 * call process_spawn_and_wait(), which holds BOTH the caller's slot and
 * its spawned child's slot simultaneously while blocked -- worst-case
 * concurrent usage is well above the persistent-thread count alone, and
 * a tight ceiling here caused sporadic "too many threads" boot panics
 * once enough self-tests could line up mid-flight at once. */
#define SCHEDULER_MAX_THREADS 32u
#define SCHEDULER_STACK_SIZE (16u * 1024u)

/* Simple preemptive round-robin scheduler over a fixed table of kernel
 * threads, each with its own heap-allocated stack and its own CR3.
 * Driven by the timer IRQ via timer_set_tick_callback(schedule) -- see
 * timer.h. */
void scheduler_init(void);

/* Allocates a stack and adds a new READY thread using the kernel's own
 * address space; returns its id. entry must never return (there's
 * nothing valid to return to). */
int thread_create(const char *name, void (*entry)(void));

/* Same, but the thread runs under its own address space (see
 * vmm_create_address_space()) instead of the kernel's -- for a thread
 * that's going to enter_usermode() into an isolated process. */
int thread_create_process(const char *name, void (*entry)(void), uint64_t cr3);

/* Same as thread_create_process(), but `entry` receives `arg` (passed
 * through rdi on its very first run -- see thread_trampoline in
 * context_switch.S) instead of taking no arguments. Needed when the
 * same launcher function is reused to start several distinct loaded
 * programs, each with its own entry point/stack (see elf_loader.h),
 * rather than a dedicated static launcher per program.
 *
 * `exit_code_out`, if non-NULL, receives this thread's eventual
 * thread_exit_code() argument -- written at the moment it exits,
 * before its slot becomes reusable. Must point at memory that outlives
 * the thread (e.g. a waiting caller's own stack frame, as in
 * process_spawn_and_wait()); pass NULL for a fire-and-forget thread
 * nobody's waiting on. */
int thread_create_process_arg(const char *name, void (*entry)(void *arg), void *arg, uint64_t cr3,
                               int64_t *exit_code_out);

/* Round-robins to the next ready thread. A no-op with 0 or 1 active
 * threads. Safe to call from within the timer ISR's call chain. */
void schedule(void);

/* Ends the calling thread: marks its slot free for reuse and never
 * returns to it. Its stack is freed once the scheduler has switched
 * away from it (not before -- see the comment in schedule()). Replaces
 * the "loop yielding forever" a thread used to do on exit now that the
 * scheduler can actually remove a thread from the round-robin. */
__attribute__((noreturn)) void thread_exit(void);

/* Same as thread_exit(), but delivers `code` to whatever
 * `exit_code_out` was given at creation time (see
 * thread_create_process_arg()) before the slot is reaped -- thread_exit()
 * is just thread_exit_code(0). Used by SYS_EXIT (the process's own
 * reported code) and by the CPU-exception handler (a synthesized code
 * identifying which fault killed it), so a crashing or misbehaving
 * process takes down only itself, not the whole kernel. */
__attribute__((noreturn)) void thread_exit_code(int64_t code);

/* True if `tid` (an id returned by one of the thread_create* functions)
 * is still running. Lets a caller block until a spawned process
 * finishes (see process_spawn_and_wait() in process.h) just by looping
 * on this and schedule() -- no separate wait queue needed. */
int thread_is_alive(int tid);

/* The currently-running thread's SYS_SBRK break (see syscall.c) --
 * every thread gets one (initialized to USER_HEAP_VADDR_START when
 * created), even though only ring-3 processes ever actually call
 * SYS_SBRK; a plain kernel thread simply never touches it. */
uint64_t thread_get_heap_brk(void);
void thread_set_heap_brk(uint64_t brk);

/* Switches from the caller's context into the first thread and never
 * returns. Call once, after creating at least one thread. */
__attribute__((noreturn)) void scheduler_start(void);

#endif /* REBORNOS_SCHEDULER_H */
