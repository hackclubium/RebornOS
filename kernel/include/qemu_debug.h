#ifndef REBORNOS_QEMU_DEBUG_H
#define REBORNOS_QEMU_DEBUG_H

#include <stdint.h>

/* QEMU's isa-debug-exit device (iobase=0xf4, configured in
 * tools/run-qemu.sh and tools/test-qemu.sh) turns a byte written to
 * port 0xf4 into the process exit code (value << 1) | 1. We use our
 * own two codes and check for them from the host side:
 *   wrote 0x00 -> qemu exits with code 1 -> test PASS
 *   wrote 0x01 -> qemu exits with code 3 -> test FAIL
 * Any other exit code means QEMU crashed/reset instead of reaching
 * this call, which is itself a test failure. */
#define QEMU_DEBUG_EXIT_SUCCESS 0x00
#define QEMU_DEBUG_EXIT_FAILURE 0x01

__attribute__((noreturn)) void qemu_debug_exit(uint8_t code);

#endif /* REBORNOS_QEMU_DEBUG_H */
