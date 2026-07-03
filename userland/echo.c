/* Minimal argv demo: prints every argument it was given, one per line.
 * Proves SYS_EXEC's argument passing actually reaches a real, separate
 * process -- argc/argv arrive in _start's rdi/rsi exactly like an
 * ordinary SysV call (see enter_usermode() in kernel/src/gdt_asm.S),
 * no crt0 needed. The syscall numbers below must match
 * kernel/include/syscall.h. */
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

void _start(int argc, char **argv) {
    for (int i = 0; i < argc; i++) {
        sys_write(argv[i]);
        sys_write("\n");
    }
    sys_exit(0);
}
