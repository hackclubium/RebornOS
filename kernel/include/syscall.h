#ifndef REBORNOS_SYSCALL_H
#define REBORNOS_SYSCALL_H

#include <stdint.h>

#define SYS_WRITE      1 /* rdi = const char* (NUL-terminated) -> writes to serial via kprintf */
#define SYS_EXIT       2 /* rdi = exit code -> ends the calling thread (see thread_exit()) */
#define SYS_READ_CHAR  3 /* no args -> blocks until a keystroke is available, returns it in rax */
#define SYS_EXEC       4 /* rdi = char** argv (argv[0] is the program path), rsi = argc ->
                           * loads and runs it with that argv, blocking until it exits;
                           * rax = 0 on success, -1 if the program wasn't found */
#define SYS_LIST_DIR   5 /* rdi = const char* path (NULL/"" for root), rsi = char* buffer,
                           * rdx = buffer size -> fills buffer with that directory's filenames
                           * (one per line); rax = bytes written, or -1 if path isn't a directory */
#define SYS_READ_FILE  6 /* rdi = const char* path, rsi = char* buffer, rdx = buffer size ->
                           * rax = bytes read, or -1 if the file doesn't exist */
#define SYS_WRITE_FILE 7 /* rdi = const char* path, rsi = const void* data, rdx = size ->
                           * creates/overwrites the file (root directory only); rax = 0 on success */

/* Observable from kmain's test-mode monitor thread: how many SYS_WRITE
 * calls the kernel has actually serviced, proving real ring3->ring0
 * round trips happened rather than the user thread just running its
 * own loop uninterrupted. */
extern volatile uint64_t syscall_write_count;

#endif /* REBORNOS_SYSCALL_H */
