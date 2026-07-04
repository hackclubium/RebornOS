/*
 * One tiny stub per interrupt vector, all funneling into a single
 * common handler. This is the one place in the kernel where assembly
 * is unavoidable: the CPU's interrupt calling convention (some vectors
 * push an error code, some don't; general-purpose registers aren't
 * saved automatically) has to be handled exactly right before any C
 * code can safely run.
 */

.macro ISR_NOERR num
.global isr_stub_\num
isr_stub_\num:
    push $0        /* dummy error code, so every frame has the same shape */
    push $\num
    jmp isr_common_stub
.endm

.macro ISR_ERR num
.global isr_stub_\num
isr_stub_\num:
    /* CPU already pushed a real error code for this vector */
    push $\num
    jmp isr_common_stub
.endm

/* CPU exceptions 0-31. Vectors that push a hardware error code
 * (8, 10-14, 17, 21, 29, 30 per the Intel SDM) use ISR_ERR; every
 * other vector, including all "reserved" ones, uses ISR_NOERR so the
 * dummy-error-code slot keeps every frame the same shape. */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* IRQ0-15, remapped to vectors 32-47 -- see pic_remap() in timer.c.
 * Every line gets a stub even though today only IRQ0 (timer), IRQ1
 * (keyboard), and IRQ12 (mouse) have a real handler registered (see
 * idt_set_irq_handler()) -- interrupt_dispatch() already no-ops
 * gracefully for an unregistered IRQ, but *every* vector a real PIC
 * line can raise needs a valid IDT gate at all, or the CPU can't even
 * deliver the interrupt and raises a #GP referencing the missing gate
 * instead (exactly what happened before this existed for IRQ12). */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

/* Vector 48: the LAPIC periodic timer every AP starts (see
 * lapic_timer_start() in lapic.c) -- outside the legacy IRQ range
 * above since it needs a LAPIC EOI, not a PIC one; idt.c's
 * interrupt_dispatch() special-cases it directly to lapic_timer_isr()
 * rather than routing it through irq_handlers[]. */
ISR_NOERR 48

/* Syscall vector (int $0x80). Deliberately NOT added to isr_stub_table
 * below (that table is sized/indexed for vectors 0-48 only) -- idt.c
 * registers this one directly by name, with a DPL=3 gate so user mode
 * is actually allowed to execute `int $0x80` without a #GP. */
ISR_NOERR 128

.extern interrupt_dispatch

isr_common_stub:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, %rdi   /* arg0 = interrupt_frame_t* (SysV: first arg in rdi) */
    call interrupt_dispatch

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $16, %rsp    /* discard vector + error_code */
    iretq

.section .data
.global isr_stub_table
isr_stub_table:
    .quad isr_stub_0
    .quad isr_stub_1
    .quad isr_stub_2
    .quad isr_stub_3
    .quad isr_stub_4
    .quad isr_stub_5
    .quad isr_stub_6
    .quad isr_stub_7
    .quad isr_stub_8
    .quad isr_stub_9
    .quad isr_stub_10
    .quad isr_stub_11
    .quad isr_stub_12
    .quad isr_stub_13
    .quad isr_stub_14
    .quad isr_stub_15
    .quad isr_stub_16
    .quad isr_stub_17
    .quad isr_stub_18
    .quad isr_stub_19
    .quad isr_stub_20
    .quad isr_stub_21
    .quad isr_stub_22
    .quad isr_stub_23
    .quad isr_stub_24
    .quad isr_stub_25
    .quad isr_stub_26
    .quad isr_stub_27
    .quad isr_stub_28
    .quad isr_stub_29
    .quad isr_stub_30
    .quad isr_stub_31
    .quad isr_stub_32
    .quad isr_stub_33
    .quad isr_stub_34
    .quad isr_stub_35
    .quad isr_stub_36
    .quad isr_stub_37
    .quad isr_stub_38
    .quad isr_stub_39
    .quad isr_stub_40
    .quad isr_stub_41
    .quad isr_stub_42
    .quad isr_stub_43
    .quad isr_stub_44
    .quad isr_stub_45
    .quad isr_stub_46
    .quad isr_stub_47
    .quad isr_stub_48
