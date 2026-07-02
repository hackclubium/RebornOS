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
 * ends up here after switch_context's register pops.
 */
.global thread_trampoline
thread_trampoline:
    sti
    call *%rbx
    /* Thread entry points loop forever and must never return; if one
     * does, there's nothing valid to return to. */
1:
    hlt
    jmp 1b
