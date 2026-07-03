#include <stdint.h>
#include "kprintf.h"
#include "serial.h"
#include "spinlock.h"

/* The UART is shared hardware state -- once more than one core can
 * genuinely be running at once (see smp.c), two cores calling
 * kprintf() concurrently interleave their bytes on the wire with no
 * lock protecting it. This is the first thing SMP actually needed a
 * real cross-core lock for. */
static spinlock_t serial_lock = SPINLOCK_INIT;

static void print_unsigned(uint64_t v, int base, int uppercase) {
    char buf[32];
    int i = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) {
        serial_write_char('0');
        return;
    }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = digits[v % (uint64_t)base];
        v /= (uint64_t)base;
    }
    while (i > 0) {
        serial_write_char(buf[--i]);
    }
}

static void print_signed(int64_t v) {
    if (v < 0) {
        serial_write_char('-');
        print_unsigned((uint64_t)(-v), 10, 0);
    } else {
        print_unsigned((uint64_t)v, 10, 0);
    }
}

void kvprintf(const char *fmt, va_list args) {
    spinlock_acquire(&serial_lock);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            serial_write_char(*p);
            continue;
        }

        p++;
        int is_long = 0;
        if (*p == 'l') {
            is_long = 1;
            p++;
        }

        switch (*p) {
            case 's': {
                const char *s = va_arg(args, const char *);
                serial_write(s ? s : "(null)");
                break;
            }
            case 'c':
                serial_write_char((char)va_arg(args, int));
                break;
            case 'd':
                if (is_long) {
                    print_signed(va_arg(args, int64_t));
                } else {
                    print_signed(va_arg(args, int));
                }
                break;
            case 'u':
                if (is_long) {
                    print_unsigned(va_arg(args, uint64_t), 10, 0);
                } else {
                    print_unsigned(va_arg(args, unsigned int), 10, 0);
                }
                break;
            case 'x':
                if (is_long) {
                    print_unsigned(va_arg(args, uint64_t), 16, 0);
                } else {
                    print_unsigned(va_arg(args, unsigned int), 16, 0);
                }
                break;
            case 'p':
                serial_write("0x");
                print_unsigned((uint64_t)(uintptr_t)va_arg(args, void *), 16, 0);
                break;
            case '%':
                serial_write_char('%');
                break;
            case '\0':
                p--; /* trailing '%' at end of string, stop cleanly */
                break;
            default:
                serial_write_char('%');
                serial_write_char(*p);
                break;
        }
    }
    spinlock_release(&serial_lock);
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
}
