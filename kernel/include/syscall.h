#ifndef REBORNOS_SYSCALL_H
#define REBORNOS_SYSCALL_H

#include <stdint.h>

#define SYS_WRITE     1 /* rdi = const char* (NUL-terminated) -> writes to serial via kprintf */
#define SYS_EXIT      2 /* rdi = exit code -> ends the calling thread (see thread_exit()) */
#define SYS_READ_CHAR 3 /* no args -> blocks until a keystroke is available, returns it in rax */
#define SYS_EXEC      4 /* rdi = const char* program name -> loads and runs it, blocking until it
                          * exits; rax = 0 on success, -1 if the program wasn't found */
#define SYS_LIST_ROOT 5 /* rdi = char* buffer, rsi = buffer size -> fills buffer with the root
                          * directory's filenames (one per line); rax = bytes written */

/* Observable from kmain's test-mode monitor thread: how many SYS_WRITE
 * calls the kernel has actually serviced, proving real ring3->ring0
 * round trips happened rather than the user thread just running its
 * own loop uninterrupted. */
extern volatile uint64_t syscall_write_count;

#endif /* REBORNOS_SYSCALL_H */
