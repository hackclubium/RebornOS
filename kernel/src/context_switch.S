/*
 * void switch_context(uint64_t *old_rsp_ptr, uint64_t new_rsp);
 *
 * Saves the SysV-ABI callee-saved registers (the caller's own
 * caller-saved registers are already protected by the normal calling
 * convention -- nothing else needs saving here), stashes the resulting
 * RSP into *old_rsp_ptr, switches RSP to new_rsp, restores that
 * thread's callee-saved registers, and `ret`s.
 *
 * This works correctly even when called from deep inside the timer
 * ISR's call chain (isr_common_stub -> interrupt_dispatch ->
 * timer_irq_handler -> schedule): from this function's point of view
 * it's an ordinary call/return, so it doesn't matter that "returning"
 * might mean resuming a completely different thread's stack, possibly
 * itself paused mid-ISR. A thread created via thread_create() has its
 * stack pre-built to look exactly like it already called this function
 * once (see scheduler.c) so the very first switch into it lands on
 * `ret` and jumps straight to its entry point.
 */
.section .text
.global switch_context
switch_context:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, (%rdi)   /* *old_rsp_ptr = rsp */
    mov %rsi, %rsp     /* rsp = new_rsp */

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

/*
 * void switch_context_release(uint64_t *old_rsp_ptr, uint64_t new_rsp,
 *                              volatile int *release_flag);
 *
 * Identical to switch_context(), except it also writes -1 through
 * release_flag -- but only *after* *old_rsp_ptr has actually been
 * stored, and before switching onto new_rsp. Real cross-core scheduling
 * (see scheduler.c) needs this ordering guarantee: a preempted thread
 * must not become visible to *another core* as "stealable" until its
 * rsp is safely persisted, or that other core could resume it from a
 * stale rsp while this core is still mid-save -- exactly the kind of
 * corruption that manifests as a jump into garbage memory. Clearing the
 * flag can't happen from plain C after calling switch_context(): the
 * code right after that call only runs again once *this exact thread*
 * is resumed, which is too late (some other core would already need to
 * have observed the flag by then). Doing it here, between the two
 * stores, works because both are plain stores from the same core in
 * program order -- x86's TSO guarantees any other core that observes
 * the second store (clearing the flag) also observes the first (the
 * rsp update) that preceded it, no explicit fence needed.
 */
.global switch_context_release
switch_context_release:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, (%rdi)   /* *old_rsp_ptr = rsp */
    movl $-1, (%rdx)   /* *release_flag = -1 -- safe now that old_rsp_ptr is stored */
    mov %rsi, %rsp     /* rsp = new_rsp */

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

/*
 * The first-ever switch into a freshly created thread lands here (see
 * scheduler.c's thread_create), not directly on the thread's entry
 * point. Why: that first switch is almost always executed from deep
 * inside the timer ISR's call chain (interrupt gates clear RFLAGS.IF
 * on entry), and since this path resumes via a plain `ret` rather than
 * the ISR's `iretq`, nothing would ever restore IF -- the new thread
 * would run with interrupts permanently disabled and never get
 * preempted again. thread_create() stashes the real entry point in the
 * rbx slot of the synthetic stack frame (rbx is otherwise unused, just
 * a convenient callee-saved register to smuggle it through), so it
 * ends up here after switch_context's register pops. The r12 slot
 * likewise carries an optional caller-supplied argument (NULL for
 * threads started via the plain thread_create()/thread_create_process()
 * API), moved into rdi -- the first SysV integer argument register --
 * right before the call.
 */
.extern thread_trampoline_null_entry_panic

.global thread_trampoline
thread_trampoline:
    sti
    mov %r12, %rdi
    /* rbx (the entry point thread_create_ex() wrote into this thread's
     * stack) should never be NULL -- if it ever is, jumping there
     * anyway produces an opaque "invalid opcode at rip=0x0" crash with
     * no context. Check first and hand off to a diagnostic panic
     * instead; this costs one cheap test+branch per thread's entire
     * lifetime (only runs once, on its very first switch-in), not per
     * context switch. */
    test %rbx, %rbx
    jnz 2f
    call thread_trampoline_null_entry_panic
2:
    call *%rbx
    /* Thread entry points loop forever and must never return; if one
     * does, there's nothing valid to return to. */
1:
    hlt
    jmp 1b
