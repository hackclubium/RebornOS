#include <stddef.h>
#include <stdint.h>
#include "boot_info.h"
#include "serial.h"
#include "framebuffer.h"
#include "panic.h"
#include "kprintf.h"
#include "qemu_debug.h"
#include "pmm.h"
#include "vmm.h"
#include "interrupts.h"
#include "timer.h"
#include "heap.h"
#include "scheduler.h"
#include "gdt.h"
#include "syscall.h"
#include "vfs.h"
#include "keyboard.h"
#include "process.h"

/* Tiny int $0x80 wrappers for the ring-3 demo program below. "+a"(ret)
 * both supplies the syscall number (via ret's initial value) and reads
 * back the kernel's return value from the same register afterward. */
static inline int64_t sys_write(const char *s) {
    int64_t ret = SYS_WRITE;
    __asm__ volatile("int $0x80" : "+a"(ret) : "D"(s) : "memory", "cc");
    return ret;
}

static inline void sys_exit(int64_t code) {
    __asm__ volatile("int $0x80" : : "a"((int64_t)SYS_EXIT), "D"(code));
    __builtin_unreachable();
}

static void ring3_program(void) {
    for (int i = 0; i < 5; i++) {
        sys_write("hello from ring3\n");
    }
    sys_exit(0);
}

/* An ordinary kernel thread whose only job is the one-way trip into
 * ring 3 -- everything after enter_usermode() is dormant until a
 * syscall or interrupt from ring3_program reactivates this thread's
 * kernel stack. */
static void user_thread_launcher(void) {
    uint8_t *user_stack = (uint8_t *)kmalloc(SCHEDULER_STACK_SIZE);
    if (user_stack == NULL) {
        panic("user_thread_launcher: kmalloc failed for the user stack");
    }
    enter_usermode(ring3_program, user_stack + SCHEDULER_STACK_SIZE, GDT_USER_CODE_SEL, GDT_USER_DATA_SEL);
}

/* Two processes proving real address-space isolation: each repeatedly
 * writes a distinct byte pattern to the exact same virtual address
 * (PROCESS_PRIVATE_VADDR) and reads it straight back. Every process's
 * page tables map that address to a *different* physical page (see
 * vmm_create_address_space()), so if isolation actually holds, neither
 * ever observes the other's value. If it didn't hold -- both somehow
 * sharing one physical page -- one would eventually read back the
 * wrong pattern and self-report failure via sys_exit(1) rather than
 * silently "passing". */
static volatile uint64_t process_a_ok = 0;
static volatile uint64_t process_b_ok = 0;

static void process_body(volatile uint64_t *ok_counter, uint8_t pattern, const char *finished_msg) {
    volatile uint8_t *private_byte = (volatile uint8_t *)PROCESS_PRIVATE_VADDR;
    for (int i = 0; i < 200; i++) {
        *private_byte = pattern;
        __asm__ volatile("pause");
        if (*private_byte != pattern) {
            sys_exit(1); /* isolation broken -- another process's write leaked in */
        }
        (*ok_counter)++;
    }
    sys_write(finished_msg);
    sys_exit(0);
}

static void process_a_entry(void) {
    process_body(&process_a_ok, 0xAA, "process A finished isolated\n");
}

static void process_b_entry(void) {
    process_body(&process_b_ok, 0xBB, "process B finished isolated\n");
}

static void process_a_launcher(void) {
    uint8_t *user_stack = (uint8_t *)kmalloc(SCHEDULER_STACK_SIZE);
    if (user_stack == NULL) {
        panic("process_a_launcher: kmalloc failed for the user stack");
    }
    enter_usermode(process_a_entry, user_stack + SCHEDULER_STACK_SIZE, GDT_USER_CODE_SEL, GDT_USER_DATA_SEL);
}

static void process_b_launcher(void) {
    uint8_t *user_stack = (uint8_t *)kmalloc(SCHEDULER_STACK_SIZE);
    if (user_stack == NULL) {
        panic("process_b_launcher: kmalloc failed for the user stack");
    }
    enter_usermode(process_b_entry, user_stack + SCHEDULER_STACK_SIZE, GDT_USER_CODE_SEL, GDT_USER_DATA_SEL);
}

/* Boots SHELL.ELF (see userland/shell.c) the same way every other
 * "real" process is started -- process_spawn() reads it off the FAT16
 * volume, maps it into a fresh address space, and hands it a thread.
 * Panics if it's missing: on a from-scratch OS with no other way to
 * launch a program, a missing shell is a boot failure, not something
 * to limp along without. */
static void boot_shell(void) {
    if (process_spawn("SHELL.ELF", "shell") < 0) {
        panic("kmain: SHELL.ELF not found on the ESP");
    }
}

#ifdef REBORNOS_TEST_MODE
/* Two worker threads that just count, plus a monitor thread that waits
 * for both to have made real progress. If the scheduler is broken --
 * wrong context-switch layout, threads never actually alternating,
 * only one ever running -- one or both counters stall and the monitor
 * hits its own bound and panics instead of hanging forever. Reaching
 * qemu_debug_exit from a scheduled thread (rather than from kmain
 * directly) is itself part of the proof: kmain handed off control via
 * scheduler_start() and never runs again. */
static volatile uint64_t worker_a_count = 0;
static volatile uint64_t worker_b_count = 0;

static void worker_a(void) {
    for (;;) {
        worker_a_count++;
        __asm__ volatile("pause");
    }
}

static void worker_b(void) {
    for (;;) {
        worker_b_count++;
        __asm__ volatile("pause");
    }
}

/* Proves process_spawn_and_wait()'s core mechanic -- spawn a program
 * off disk, cooperatively yield until it exits, observe it really did
 * finish -- from a plain kernel thread, without needing a keyboard or
 * a human typing into the real shell. This is the exact code path
 * SYS_EXEC uses; only the caller (a syscall handler vs. this thread)
 * differs. Also doubles as a thread_exit() exercise: this thread ends
 * itself the same way any other one does. */
static volatile int exec_wait_ok = 0;

static void exec_wait_test_thread(void) {
    if (process_spawn_and_wait("INIT.ELF", "exec-test") != 0) {
        panic("exec/thread-exit self-test: INIT.ELF not found");
    }
    exec_wait_ok = 1;
    thread_exit();
}

static void scheduler_monitor(void) {
    uint64_t spins = 0;
    /* syscall_write_count >= 6: ring3_program's 5 fixed writes plus
     * INIT.ELF's one write (via the exec/thread-exit self-test), both
     * deterministic regardless of exactly when process A/B's own
     * finish-messages land (see process_body -- their ok-counter and
     * their sys_write race against each other, so their contribution
     * to this count isn't guaranteed at any given instant, but these
     * two are). */
    while (worker_a_count < 1000 || worker_b_count < 1000 || syscall_write_count < 6 ||
           process_a_ok < 200 || process_b_ok < 200 || exec_wait_ok < 1) {
        spins++;
        if (spins > 4000000000ULL) {
            panic("scheduler/syscall/isolation self-test: no progress "
                  "(a=%lu b=%lu syscalls=%lu proc_a=%lu proc_b=%lu exec=%d)",
                  worker_a_count, worker_b_count, syscall_write_count, process_a_ok, process_b_ok,
                  exec_wait_ok);
        }
    }
    kprintf("TEST MODE: scheduler+syscall+isolation self-test passed "
            "(a=%lu b=%lu syscalls=%lu proc_a=%lu proc_b=%lu)\n",
            worker_a_count, worker_b_count, syscall_write_count, process_a_ok, process_b_ok);
    qemu_debug_exit(QEMU_DEBUG_EXIT_SUCCESS);
}
#else
static void idle_thread(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}
#endif

void kmain(boot_info_t *info) {
    serial_init();
    kprintf("RebornOS kernel: serial online\n");

    if (info == NULL || info->magic != BOOT_INFO_MAGIC) {
        panic("invalid boot_info handoff (magic mismatch)");
    }

    kprintf("boot_info OK: framebuffer %ux%u @ 0x%lx (%lu bytes)\n",
            info->framebuffer.width, info->framebuffer.height,
            info->framebuffer.base, info->framebuffer.size);
    kprintf("memory map: %lu bytes, descriptor size %lu, version %u\n",
            info->memory_map_size, info->memory_map_descriptor_size,
            info->memory_map_descriptor_version);

    fb_init(&info->framebuffer);
    fb_clear(0x00102030);
    fb_puts(8, 8, "REBORNOS -- MILESTONE 6: INTERACTIVE SHELL", 0xFFFFFFFF, 0x00102030);
    fb_puts(8, 24, "SERIAL + FRAMEBUFFER ALIVE.", 0xFFA0A0A0, 0x00102030);

    kprintf("kmain: boot checks complete\n");

    gdt_init();
    pmm_init(info);
    vmm_init();
    idt_init();
    keyboard_init();
    timer_init(100);
    heap_init();
    vfs_init();

#ifdef REBORNOS_TEST_MODE
    /* Exercise the physical allocator: distinct pages, independently
     * writable, freed pages become reusable. Any failure panics
     * instead of silently reporting success. */
    void *p1 = pmm_alloc_page();
    void *p2 = pmm_alloc_page();
    if (p1 == NULL || p2 == NULL || p1 == p2) {
        panic("pmm self-test: alloc returned null or a duplicate page");
    }

    *(volatile uint8_t *)p1 = 0xAB;
    *(volatile uint8_t *)p2 = 0xCD;
    if (*(volatile uint8_t *)p1 != 0xAB || *(volatile uint8_t *)p2 != 0xCD) {
        panic("pmm self-test: allocated pages are not independently writable");
    }

    uint64_t free_before = pmm_free_page_count();
    pmm_free_page(p1);
    pmm_free_page(p2);
    if (pmm_free_page_count() != free_before + 2) {
        panic("pmm self-test: free count didn't increase after freeing pages");
    }

    void *p3 = pmm_alloc_page();
    if (p3 == NULL) {
        panic("pmm self-test: re-alloc after free failed");
    }
    kprintf("TEST MODE: pmm self-test passed\n");

    /* Exercise interrupts: if idt_init()/timer_init() are wrong in any
     * way that breaks IRQ delivery, this spins forever instead of
     * silently "passing" -- bounded so a real failure still reports
     * FAIL via the test harness's own timeout rather than hanging the
     * CI runner indefinitely. */
    uint64_t start_ticks = timer_ticks();
    uint64_t spins = 0;
    while (timer_ticks() == start_ticks) {
        __asm__ volatile("pause");
        spins++;
        if (spins > 500000000ULL) {
            panic("timer self-test: no tick observed -- IDT/PIC/PIT wiring is broken");
        }
    }
    kprintf("TEST MODE: timer self-test passed (%lu tick(s) observed)\n", timer_ticks());

    /* Exercise the heap: distinct allocations, independently writable,
     * a freed block gets reused, an oversized request fails cleanly
     * instead of corrupting something. */
    uint64_t heap_free_before = heap_free_bytes();
    char *h1 = (char *)kmalloc(64);
    char *h2 = (char *)kmalloc(128);
    if (h1 == NULL || h2 == NULL || (void *)h1 == (void *)h2) {
        panic("heap self-test: kmalloc returned null or overlapping blocks");
    }
    for (int i = 0; i < 64; i++) {
        h1[i] = (char)0xAB;
    }
    for (int i = 0; i < 128; i++) {
        h2[i] = (char)0xCD;
    }
    for (int i = 0; i < 64; i++) {
        if (h1[i] != (char)0xAB) {
            panic("heap self-test: block 1 contents corrupted (overlap with block 2?)");
        }
    }
    for (int i = 0; i < 128; i++) {
        if (h2[i] != (char)0xCD) {
            panic("heap self-test: block 2 contents corrupted");
        }
    }
    /* Free in reverse (LIFO) allocation order: kfree only coalesces
     * forward, so freeing h2 first lets it merge into the free block
     * after it, and freeing h1 second then merges it into that now-
     * larger neighbor -- fully coalescing back into a single block.
     * Freeing in allocation order instead would leave h1's 24-byte
     * header permanently un-mergeable and this assertion would be
     * wrong, not the allocator. */
    kfree(h2);
    kfree(h1);
    if (heap_free_bytes() != heap_free_before) {
        panic("heap self-test: free byte count didn't return to baseline after freeing everything");
    }
    void *huge = kmalloc(1024ULL * 1024 * 1024); /* far bigger than the whole heap */
    if (huge != NULL) {
        panic("heap self-test: an allocation bigger than the entire heap should fail, not succeed");
    }
    kprintf("TEST MODE: heap self-test passed (%lu bytes free)\n", heap_free_bytes());

    /* Exercise the keyboard driver's buffer + translation path without
     * needing a real keystroke: inject characters the same way a real
     * scancode's translated ASCII would land in the ring buffer, then
     * read them back in order and confirm the buffer reports empty
     * once drained. */
    keyboard_inject_char('h');
    keyboard_inject_char('i');
    keyboard_inject_char('\n');
    if (keyboard_read_char() != 'h' || keyboard_read_char() != 'i' || keyboard_read_char() != '\n') {
        panic("keyboard self-test: characters didn't round-trip in order");
    }
    if (keyboard_read_char() != -1) {
        panic("keyboard self-test: buffer should be empty after reading everything back");
    }
    kprintf("TEST MODE: keyboard self-test passed\n");

    kprintf("TEST MODE: starting scheduler+syscall+isolation self-test\n");
    scheduler_init();
    thread_create("worker-a", worker_a);
    thread_create("worker-b", worker_b);
    thread_create("monitor", scheduler_monitor);
    thread_create("user", user_thread_launcher);
    thread_create_process("process-a", process_a_launcher, vmm_create_address_space());
    thread_create_process("process-b", process_b_launcher, vmm_create_address_space());
    thread_create("exec-wait-test", exec_wait_test_thread);
    boot_shell();
    timer_set_tick_callback(schedule);
    scheduler_start();
    panic("kmain: scheduler_start returned -- unreachable");
#else
    {
        uint64_t total_mib = (pmm_total_pages() * PMM_PAGE_SIZE) / (1024 * 1024);
        uint64_t free_mib = (pmm_free_page_count() * PMM_PAGE_SIZE) / (1024 * 1024);
        kprintf("pmm ready: %lu MiB total, %lu MiB free\n", total_mib, free_mib);
    }

    kprintf("kmain: starting scheduler\n");
    scheduler_init();
    thread_create("idle", idle_thread);
    thread_create("user", user_thread_launcher);
    thread_create_process("process-a", process_a_launcher, vmm_create_address_space());
    thread_create_process("process-b", process_b_launcher, vmm_create_address_space());
    boot_shell();
    timer_set_tick_callback(schedule);
    scheduler_start();
    panic("kmain: scheduler_start returned -- unreachable");
#endif
}
