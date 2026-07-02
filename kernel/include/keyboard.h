#ifndef REBORNOS_KEYBOARD_H
#define REBORNOS_KEYBOARD_H

/* A minimal PS/2 keyboard driver: US QWERTY, scancode set 1 (what the
 * i8042 controller normalizes every keyboard down to, including
 * QEMU's emulated one), lowercase only. Shift/Ctrl/Alt scancodes are
 * recognized just enough to be ignored rather than corrupting the
 * output -- real shifted/symbol input is future work. That's enough to
 * type a filename (fat16.c uppercases everything when matching anyway)
 * and run a command like `ls`.
 *
 * Registers its own IRQ1 handler -- call after idt_init(). */
void keyboard_init(void);

/* Returns the next buffered character, or -1 if none is available yet.
 * Never blocks itself; callers that want to block (see SYS_READ_CHAR
 * in syscall.c) loop calling the scheduler until this returns
 * something. */
int keyboard_read_char(void);

/* Pushes a character into the same ring buffer real keystrokes land in,
 * bypassing scancode translation entirely -- lets TEST_MODE script a
 * "typed" line deterministically without faking hardware scancodes. */
void keyboard_inject_char(char c);

#endif /* REBORNOS_KEYBOARD_H */
