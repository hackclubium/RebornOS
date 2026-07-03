#ifndef REBORNOS_GDT_H
#define REBORNOS_GDT_H

#include <stdint.h>

/* Selector values (GDT byte offset, RPL baked into the low 2 bits for
 * the user ones since every use of them needs RPL=3 anyway). */
#define GDT_KERNEL_CODE_SEL 0x08u
#define GDT_KERNEL_DATA_SEL 0x10u
#define GDT_USER_DATA_SEL   (0x18u | 3u)
#define GDT_USER_CODE_SEL   (0x20u | 3u)
#define GDT_TSS_SEL         0x28u

/* Builds and loads our own GDT (replacing UEFI's) plus a TSS whose
 * RSP0 is a dedicated kernel stack for privilege-elevation on
 * interrupt/exception/syscall entry from ring 3. Must run before
 * idt_init(), which reads the *current* CS to populate IDT gates. */
void gdt_init(void);

/* TSS.RSP0 is *the* kernel stack the CPU switches to on any ring3->
 * ring0 transition -- there's only one such register, so with more
 * than one ring-3-capable thread it must be updated to the currently
 * running thread's own kernel stack on every context switch, or two
 * threads trapping into the kernel at different times end up sharing
 * (and corrupting) the same stack memory. Called from scheduler.c. */
void tss_set_kernel_stack(uint64_t rsp0);

/* Builds a fake interrupt-return frame for (entry, user_stack_top,
 * user_cs, user_ds) and executes iretq, dropping into ring 3 for the
 * first time. Never returns in the normal sense -- from here on this
 * thread's kernel-mode call stack is dormant until an interrupt or
 * syscall from ring 3 reactivates it.
 *
 * argc/argv are handed to the new process's entry point via rdi/rsi,
 * exactly like an ordinary SysV call -- pass 0/NULL for a process that
 * doesn't take arguments (see elf_loader.h for how a real argv gets
 * built for one that does). */
__attribute__((noreturn)) void enter_usermode(void (*entry)(void), void *user_stack_top,
                                               uint16_t user_cs, uint16_t user_ds,
                                               int argc, char **argv);

#endif /* REBORNOS_GDT_H */
