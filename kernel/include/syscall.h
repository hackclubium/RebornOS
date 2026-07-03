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

/* rdi = a 20-byte buffer laid out as:
 *   uint32_t width, height, pixels_per_scanline, bytes_per_pixel;
 *   uint8_t  red_shift, green_shift, blue_shift, _pad;
 * Fills it in; rax = 0. Userland programs don't share this header (see
 * every existing userland/*.c), so any caller redeclares this layout
 * itself -- see userland/gfxtest.c. */
#define SYS_FB_INFO 8

/* No args. Maps the framebuffer's physical pages into the calling
 * process's own address space at a fixed vaddr (USER_FB_VADDR, see
 * elf_loader.h) -- idempotent, safe to call more than once. rax = that
 * vaddr. A process writes pixels there directly (no per-pixel syscall
 * overhead); use SYS_FB_INFO first to know the stride/format. */
#define SYS_FB_MAP 9

/* rdi = a 12-byte buffer laid out as:
 *   int32_t x, y; uint8_t buttons; uint8_t _pad[3];
 * (buttons: bit0=left, bit1=right, bit2=middle). Fills it in; rax = 0. */
#define SYS_MOUSE_READ 10

/* rdi = increment (bytes, must be >= 0 -- shrinking isn't supported).
 * Grows the calling process's heap by that many bytes, demand-mapping
 * whatever new pages that requires. rax = the break *before* this
 * call (matching classic sbrk() semantics -- that's the start of the
 * newly available region), or -1 if out of physical memory. */
#define SYS_SBRK 11

/* Observable from kmain's test-mode monitor thread: how many SYS_WRITE
 * calls the kernel has actually serviced, proving real ring3->ring0
 * round trips happened rather than the user thread just running its
 * own loop uninterrupted. */
extern volatile uint64_t syscall_write_count;

#endif /* REBORNOS_SYSCALL_H */
