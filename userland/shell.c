/* RebornOS's shell: a real ELF64 executable loaded off disk at boot
 * (see process_spawn() in kernel/src/process.c), same as any other
 * program -- there's nothing kernel-side about it beyond the syscalls
 * it uses. Reads a line of keyboard input, echoing as it goes, and
 * either lists the root directory (`ls`) or hands the line to SYS_EXEC
 * as a program name. No libc, no crt0 -- see userland/init.c for why
 * that's fine.
 *
 * The syscall numbers below must match kernel/include/syscall.h --
 * this program is a standalone link unit and can't #include that
 * kernel header. */
#define SYS_WRITE     1
#define SYS_EXIT      2
#define SYS_READ_CHAR 3
#define SYS_EXEC      4
#define SYS_LIST_ROOT 5

static inline long sys_write(const char *s) {
    long ret = SYS_WRITE;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(s) : "memory", "cc");
    return ret;
}

static inline long sys_read_char(void) {
    long ret = SYS_READ_CHAR;
    __asm__ volatile("int $0x80" : "+a"(ret) : : "memory", "cc");
    return ret;
}

static inline long sys_exec(const char *name) {
    long ret = SYS_EXEC;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(name) : "memory", "cc");
    return ret;
}

static inline long sys_list_root(char *buf, unsigned long len) {
    long ret = SYS_LIST_ROOT;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(buf), "S"(len) : "memory", "cc");
    return ret;
}

static void echo_char(char c) {
    char s[2] = { c, 0 };
    sys_write(s);
}

static int streq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

#define LINE_MAX 64

static unsigned int read_line(char *line) {
    unsigned int len = 0;
    for (;;) {
        char c = (char)sys_read_char();
        if (c == '\n') {
            sys_write("\n");
            break;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                sys_write("\b \b"); /* move back, erase, move back again */
            }
            continue;
        }
        if (len < LINE_MAX - 1) {
            line[len++] = c;
            echo_char(c);
        }
    }
    line[len] = '\0';
    return len;
}

void _start(void) {
    char line[LINE_MAX];
    char listing[512];

    for (;;) {
        sys_write("> ");
        if (read_line(line) == 0) {
            continue;
        }

        if (streq(line, "ls")) {
            if (sys_list_root(listing, sizeof(listing)) > 0) {
                sys_write(listing);
            }
            continue;
        }

        if (sys_exec(line) != 0) {
            sys_write("shell: command not found\n");
        }
    }
}
