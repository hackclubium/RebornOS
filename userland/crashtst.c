/* Proves the kernel's fault isolation actually works (see idt.c's
 * exception_handler and scheduler.c's thread_exit_code()): deliberately
 * executes ud2 (a guaranteed #UD), and only "succeeds" by never getting
 * the chance to report success itself -- if the kernel is still alive
 * and scheduling other threads afterward, kmain.c's crashtest_thread()
 * (the one that spawned this program) sees it get killed rather than
 * the whole system going down. No libc, no crt0 -- same convention as
 * every other userland program here.
 *
 * The syscall numbers below must match kernel/include/syscall.h. */
#define SYS_WRITE 1
#define SYS_EXIT  2

static inline long sys_write(const char *s) {
    long ret = SYS_WRITE;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(s) : "memory", "cc");
    return ret;
}

static inline void sys_exit(long code) {
    __asm__ volatile("int $0x80" : : "a"((long)SYS_EXIT), "D"(code));
    __builtin_unreachable();
}

void _start(void) {
    sys_write("crashtest: about to fault on purpose\n");

    /* ud2 is an x86 instruction architecturally guaranteed to raise
     * #UD (Invalid Opcode) -- unlike a bad memory access, this doesn't
     * depend on anything about this process's address-space layout
     * (the low identity-mapped region every process shares is
     * deliberately user-writable, see vmm_create_address_space(), so a
     * NULL-pointer write alone wouldn't actually fault here). */
    __asm__ volatile("ud2");

    /* Only reached if fault isolation is broken (ud2 above didn't
     * actually fault, or somehow returned control here). */
    sys_write("crashtest: FAULT ISOLATION FAILED -- survived ud2\n");
    sys_exit(1);
}
