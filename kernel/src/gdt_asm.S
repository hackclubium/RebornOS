/*
 * Low-level privilege/segment mechanics that can't be expressed safely
 * in inline asm: reloading CS requires a control-flow transfer (a
 * regular `mov` can't touch CS), and dropping into ring 3 for the
 * first time means building a fake interrupt-return frame by hand.
 */

.section .text

/* void reload_segments(void);
 * lgdt loads the GDTR, but the CPU only re-reads CS on a control
 * transfer -- the classic fix is a far return: push the target
 * selector and offset, then `lretq` loads both CS and RIP together. */
.global reload_segments
reload_segments:
    push $0x08              /* GDT_KERNEL_CODE_SEL */
    lea 1f(%rip), %rax
    push %rax
    lretq
1:
    mov $0x10, %ax           /* GDT_KERNEL_DATA_SEL */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    ret

/*
 * void enter_usermode(void (*entry)(void), void *user_stack_top,
 *                     uint16_t user_cs, uint16_t user_ds);
 * SysV: rdi=entry, rsi=user_stack_top, rdx=user_cs, rcx=user_ds
 *
 * `iretq` only reloads CS and SS itself; the other data segment
 * registers need an explicit mov first. Building [[SS][RSP][RFLAGS]
 * [CS][RIP]] on the stack and executing iretq is the standard way to
 * enter a lower privilege level for the first time (there's no other
 * instruction that can drop CPL going down).
 */
.global enter_usermode
enter_usermode:
    mov %cx, %ds
    mov %cx, %es
    mov %cx, %fs
    mov %cx, %gs

    push %rcx        /* SS = user_ds */
    push %rsi        /* RSP = user_stack_top */
    pushfq
    pop %rax
    or $0x200, %rax  /* force IF=1: user code must run with interrupts enabled */
    push %rax        /* RFLAGS */
    push %rdx        /* CS = user_cs */
    push %rdi        /* RIP = entry */
    iretq
