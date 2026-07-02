/* RebornOS's first real userland program: a separate ELF64 executable,
 * built with our own cross-compiler and linked to run at
 * ELF_USER_LOAD_BASE (see kernel/include/elf_loader.h), staged onto the
 * ESP as INIT.ELF, and loaded off a real FAT32 filesystem by the
 * kernel's VFS + ELF loader rather than being baked into the kernel
 * image. No libc, no crt0 -- enter_usermode() already leaves the stack
 * set up (see elf_loader.c), so _start can just be an ordinary C
 * function. Raw int $0x80 syscalls, same convention kmain.c's own
 * ring3 test programs use.
 *
 * The syscall numbers below must match kernel/include/syscall.h's
 * SYS_WRITE/SYS_EXIT -- this program is a standalone link unit and
 * can't #include that kernel header. */
#define SYS_WRITE 1
#define SYS_EXIT 2

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
    sys_write("init: loaded from disk via FAT32 + ELF loader\n");
    sys_exit(0);
}
