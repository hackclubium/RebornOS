#include <stdint.h>
#include "spinlock.h"

/* Test-and-test-and-set: spin on a plain read (cheap, keeps the cache
 * line shared) until the lock *looks* free, then try the actual atomic
 * claim -- avoids hammering the bus with locked instructions while
 * waiting, the classic reason to test before test-and-set. `lock
 * xchg` is used for the claim itself (xchg is implicitly locked on
 * x86, no explicit `lock` prefix needed) since it atomically both
 * reads the previous value and writes 1: if it reads back 0, we're the
 * one who just claimed it. */
void spinlock_acquire(spinlock_t *lock) {
    for (;;) {
        uint32_t prev = 1;
        __asm__ volatile("xchg %0, %1" : "+r"(prev), "+m"(lock->locked) : : "memory");
        if (prev == 0) {
            return;
        }
        while (lock->locked) {
            __asm__ volatile("pause");
        }
    }
}

void spinlock_release(spinlock_t *lock) {
    __asm__ volatile("" ::: "memory"); /* compiler barrier -- don't let prior writes reorder past this */
    lock->locked = 0;
}
