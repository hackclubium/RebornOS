#ifndef REBORNOS_SYSCALL_H
#define REBORNOS_SYSCALL_H

#include <stdint.h>

#define SYS_WRITE 1 /* rdi = const char* (NUL-terminated) -> writes to serial via kprintf */
#define SYS_EXIT  2 /* rdi = exit code -> halts the calling thread */

/* Observable from kmain's test-mode monitor thread: how many SYS_WRITE
 * calls the kernel has actually serviced, proving real ring3->ring0
 * round trips happened rather than the user thread just running its
 * own loop uninterrupted. */
extern volatile uint64_t syscall_write_count;

#endif /* REBORNOS_SYSCALL_H */
