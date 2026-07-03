/* RebornOS's shell: a real ELF64 executable loaded off disk at boot
 * (see process_spawn() in kernel/src/process.c), same as any other
 * program -- there's nothing kernel-side about it beyond the syscalls
 * it uses. Reads a line of keyboard input, echoing as it goes,
 * tokenizes it into argv[], and either runs a builtin (`ls`, `cat`,
 * `write`) or hands argv to SYS_EXEC as a program invocation. No libc,
 * no crt0 -- see userland/init.c for why that's fine.
 *
 * The syscall numbers below must match kernel/include/syscall.h --
 * this program is a standalone link unit and can't #include that
 * kernel header. */
#define SYS_WRITE      1
#define SYS_EXIT       2
#define SYS_READ_CHAR  3
#define SYS_EXEC       4
#define SYS_LIST_DIR   5
#define SYS_READ_FILE  6
#define SYS_WRITE_FILE 7

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

static inline long sys_exec(char **argv, long argc) {
    long ret = SYS_EXEC;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(argv), "S"(argc) : "memory", "cc");
    return ret;
}

static inline long sys_list_dir(const char *path, char *buf, unsigned long len) {
    long ret = SYS_LIST_DIR;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(path), "S"(buf), "d"(len) : "memory", "cc");
    return ret;
}

static inline long sys_read_file(const char *path, char *buf, unsigned long len) {
    long ret = SYS_READ_FILE;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(path), "S"(buf), "d"(len) : "memory", "cc");
    return ret;
}

static inline long sys_write_file(const char *path, const char *data, unsigned long len) {
    long ret = SYS_WRITE_FILE;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(path), "S"(data), "d"(len) : "memory", "cc");
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

#define LINE_MAX 128
#define MAX_ARGS 16
#define IOBUF_SIZE 512

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

/* Splits `line` in place on spaces (each separating space becomes a
 * NUL, and argv[i] points into the middle of `line` rather than a
 * copy) -- good enough for a shell with no quoting support. */
static int tokenize(char *line, char *argv[MAX_ARGS]) {
    int argc = 0;
    char *p = line;
    while (*p != '\0' && argc < MAX_ARGS) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        if (*p == ' ') {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

void _start(void) {
    char line[LINE_MAX];
    char iobuf[IOBUF_SIZE];
    char *argv[MAX_ARGS];

    for (;;) {
        sys_write("> ");
        if (read_line(line) == 0) {
            continue;
        }

        int argc = tokenize(line, argv);
        if (argc == 0) {
            continue;
        }

        if (streq(argv[0], "ls")) {
            const char *path = argc > 1 ? argv[1] : "";
            long n = sys_list_dir(path, iobuf, sizeof(iobuf));
            if (n > 0) {
                sys_write(iobuf);
            } else if (n < 0) {
                sys_write("ls: not found\n");
            }
            continue;
        }

        if (streq(argv[0], "cat")) {
            if (argc < 2) {
                sys_write("usage: cat <file>\n");
                continue;
            }
            long n = sys_read_file(argv[1], iobuf, sizeof(iobuf) - 1);
            if (n < 0) {
                sys_write("cat: not found\n");
                continue;
            }
            iobuf[n] = '\0';
            sys_write(iobuf);
            sys_write("\n");
            continue;
        }

        if (streq(argv[0], "write")) {
            if (argc < 2) {
                sys_write("usage: write <file> [text...]\n");
                continue;
            }
            /* Join argv[2..] back into one space-separated string --
             * tokenize() already turned the original spaces into NULs,
             * so multiple spaces between words collapse to one. */
            unsigned int len = 0;
            for (int i = 2; i < argc && len < sizeof(iobuf) - 1; i++) {
                if (i > 2) {
                    iobuf[len++] = ' ';
                }
                for (const char *s = argv[i]; *s != '\0' && len < sizeof(iobuf) - 1; s++) {
                    iobuf[len++] = *s;
                }
            }
            if (sys_write_file(argv[1], iobuf, len) != 0) {
                sys_write("write: failed\n");
            }
            continue;
        }

        /* Every program is staged on disk as an uppercase NAME.ELF (see
         * tools/mkimage.sh) and fat16.c matches names exactly -- typing
         * the bare command name like a normal shell (`echo`, not
         * `echo.elf`) would otherwise always miss. Append .elf when the
         * user didn't already type an extension. */
        char exec_name[LINE_MAX + 4];
        int has_dot = 0;
        for (const char *s = argv[0]; *s != '\0'; s++) {
            if (*s == '.') {
                has_dot = 1;
                break;
            }
        }
        if (!has_dot) {
            unsigned int i = 0;
            for (; argv[0][i] != '\0' && i < sizeof(exec_name) - 5; i++) {
                exec_name[i] = argv[0][i];
            }
            const char *suffix = ".elf";
            for (int j = 0; suffix[j] != '\0'; j++) {
                exec_name[i++] = suffix[j];
            }
            exec_name[i] = '\0';
            argv[0] = exec_name;
        }

        if (sys_exec(argv, argc) != 0) {
            sys_write("shell: command not found\n");
        }
    }
}
