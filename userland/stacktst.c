/* Proves elf_handle_stack_fault() (kernel/src/elf_loader.c) actually
 * works: the ELF loader only maps the topmost stack page eagerly (see
 * ELF_USER_STACK_PAGES in kernel/include/elf_loader.h) -- everything
 * below that is supposed to appear on demand, one page-fault at a
 * time, up to ELF_USER_STACK_MAX_PAGES. Recursing deep enough to blow
 * past that first page, with each frame verifying its own locals
 * survived the calls below it, is the most direct way to demonstrate
 * that demand paging is really happening rather than just not being
 * exercised. No libc, no crt0 -- same convention as every other
 * userland program here.
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

/* 400 levels * a 256-byte frame each is comfortably past the single
 * 4 KiB page mapped eagerly, and well inside the 256 KiB the kernel
 * allows the stack to grow to -- enough to demand-page in dozens of
 * fresh stack pages without ever hitting the real limit. */
#define RECURSE_DEPTH 400
#define FRAME_BUF_SIZE 256

/* noinline so the compiler can't flatten this into a loop with no real
 * per-level stack frame -- the whole point is to actually grow the
 * stack, not just count down a register. */
__attribute__((noinline))
static int recurse(int depth) {
    volatile char buf[FRAME_BUF_SIZE];
    char marker = (char)(depth & 0xFF);
    for (int i = 0; i < FRAME_BUF_SIZE; i++) {
        buf[i] = marker;
    }

    int result = (depth == 0) ? 0 : recurse(depth - 1);

    for (int i = 0; i < FRAME_BUF_SIZE; i++) {
        if (buf[i] != marker) {
            return -1; /* this frame's locals didn't survive the calls below it */
        }
    }
    return result;
}

void _start(void) {
    int r = recurse(RECURSE_DEPTH);
    if (r == 0) {
        sys_write("stacktest: ok\n");
        sys_exit(0);
    } else {
        sys_write("stacktest: corrupted\n");
        sys_exit(1);
    }
}
