#ifndef REBORNOS_GDT_H
#define REBORNOS_GDT_H

#include <stdint.h>
#include "smp.h"

/* Selector values (GDT byte offset, RPL baked into the low 2 bits for
 * the user ones since every use of them needs RPL=3 anyway). */
#define GDT_KERNEL_CODE_SEL 0x08u
#define GDT_KERNEL_DATA_SEL 0x10u
#define GDT_USER_DATA_SEL   (0x18u | 3u)
#define GDT_USER_CODE_SEL   (0x20u | 3u)
#define GDT_TSS_SEL         0x28u /* core 0's (the BSP's) TSS selector */

/* Every core needs its *own* TSS.RSP0 -- see tss_set_kernel_stack()
 * below for why one shared register/struct isn't enough once more
 * than one core can take a ring3->ring0 transition. Each core's TSS
 * descriptor is a fixed 16 bytes further into the GDT than the last
 * (a 64-bit TSS descriptor's own size), so core i's selector is always
 * derivable from its index alone. */
#define GDT_TSS_SEL_FOR_CPU(core_index) (GDT_TSS_SEL + (uint16_t)(core_index) * 16u)

/* Builds and loads our own GDT (replacing UEFI's) plus a TSS whose
 * RSP0 is a dedicated kernel stack for privilege-elevation on
 * interrupt/exception/syscall entry from ring 3. Must run before
 * idt_init(), which reads the *current* CS to populate IDT gates.
 * Always runs on (and only ever needs to set up core 0's TSS for) the
 * BSP -- SMP doesn't exist yet at this point in boot. */
void gdt_init(void);

/* Every core has its own GDTR register even though the GDT itself is
 * one shared table in memory -- gdt_init() only loads the BSP's GDTR
 * (and only builds core 0's TSS descriptor). An AP loads its own
 * temporary GDT to get into long mode in the first place (see
 * ap_trampoline.S), then calls this once it's running real C code to
 * switch to the kernel's real, permanent GDT before doing anything
 * else (it must not keep pointing at the trampoline's temporary one,
 * which lives in low memory that could later be reused for
 * something else) -- and, unlike the BSP, an AP does need a working
 * TSS/LTR of its own the moment it can run ring-3-capable scheduled
 * threads, so this also builds and loads *this* core's TSS descriptor
 * (`core_index`, this kernel's own 0..(smp_cpu_count()-1) numbering --
 * see smp_current_cpu_index()), with a default RSP0 the scheduler
 * immediately overwrites via tss_set_kernel_stack() once this core
 * picks up its first real thread. */
void gdt_load_on_this_cpu(uint32_t core_index);

/* TSS.RSP0 is *the* kernel stack the CPU switches to on any ring3->
 * ring0 transition on `core_index` -- there's only one such register
 * per core, so with more than one ring-3-capable thread able to run on
 * that core it must be updated to the currently running thread's own
 * kernel stack on every context switch, or two threads trapping into
 * the kernel at different times (whether on the same core over time,
 * or -- now that more than one core can run scheduled threads --
 * two different cores at once) end up sharing (and corrupting) the
 * same stack memory. Called from scheduler.c. */
void tss_set_kernel_stack(uint32_t core_index, uint64_t rsp0);

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
