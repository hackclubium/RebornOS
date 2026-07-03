#include <stdint.h>
#include "gdt.h"
#include "kprintf.h"

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity; /* high nibble = flags (G, D/B or L, AVL), low nibble = limit bits 16-19 */
    uint8_t base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
    uint32_t base_upper; /* bits 32-63 -- a TSS descriptor is a "system" segment, so unlike code/data
                           * descriptors its base is a real 64-bit pointer the CPU actually uses. */
    uint32_t reserved;
} tss_descriptor_t;

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

typedef struct __attribute__((packed)) {
    gdt_entry_t entries[5]; /* null, kernel code, kernel data, user data, user code */
    tss_descriptor_t tss_desc;
} gdt_table_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

#define GDT_ACCESS_KERNEL_CODE 0x9A /* present, DPL0, code, execute/read */
#define GDT_ACCESS_KERNEL_DATA 0x92 /* present, DPL0, data, read/write */
#define GDT_ACCESS_USER_DATA   0xF2 /* present, DPL3, data, read/write */
#define GDT_ACCESS_USER_CODE   0xFA /* present, DPL3, code, execute/read */
#define GDT_ACCESS_TSS         0x89 /* present, DPL0, 64-bit TSS (available) */

#define GDT_FLAGS_LONG_MODE_CODE 0xA0 /* G=1, L=1 -- L only means something for code segments */
#define GDT_FLAGS_DATA           0xC0 /* G=1, D/B=1 */

static gdt_table_t gdt_table;
static tss_t tss;

/* TSS.RSP0's initial value, before the scheduler exists to take over
 * with tss_set_kernel_stack() per thread. Nothing runs in ring 3 this
 * early, so this default is never actually used for a real privilege
 * transition -- it just needs to be a valid pointer. */
static uint8_t syscall_stack[16384] __attribute__((aligned(16)));

static void gdt_set_entry(int i, uint8_t access, uint8_t flags) {
    gdt_table.entries[i].limit_low = 0xFFFF;
    gdt_table.entries[i].base_low = 0;
    gdt_table.entries[i].base_mid = 0;
    gdt_table.entries[i].access = access;
    gdt_table.entries[i].granularity = 0x0F | (flags & 0xF0);
    gdt_table.entries[i].base_high = 0;
}

static void tss_set_descriptor(uint64_t base, uint32_t limit) {
    gdt_table.tss_desc.limit_low = (uint16_t)(limit & 0xFFFF);
    gdt_table.tss_desc.base_low = (uint16_t)(base & 0xFFFF);
    gdt_table.tss_desc.base_mid = (uint8_t)((base >> 16) & 0xFF);
    gdt_table.tss_desc.access = GDT_ACCESS_TSS;
    gdt_table.tss_desc.granularity = (uint8_t)((limit >> 16) & 0x0F);
    gdt_table.tss_desc.base_high = (uint8_t)((base >> 24) & 0xFF);
    gdt_table.tss_desc.base_upper = (uint32_t)(base >> 32);
    gdt_table.tss_desc.reserved = 0;
}

static inline void load_gdt(gdt_ptr_t *ptr) {
    __asm__ volatile("lgdt (%0)" : : "r"(ptr));
}

static inline void load_tr(uint16_t sel) {
    __asm__ volatile("ltr %0" : : "r"(sel));
}

extern void reload_segments(void); /* gdt_asm.S: far-jumps to reload CS, then reloads DS/ES/FS/GS/SS */

void gdt_init(void) {
    gdt_set_entry(0, 0, 0); /* null descriptor */
    gdt_set_entry(1, GDT_ACCESS_KERNEL_CODE, GDT_FLAGS_LONG_MODE_CODE);
    gdt_set_entry(2, GDT_ACCESS_KERNEL_DATA, GDT_FLAGS_DATA);
    gdt_set_entry(3, GDT_ACCESS_USER_DATA, GDT_FLAGS_DATA);
    gdt_set_entry(4, GDT_ACCESS_USER_CODE, GDT_FLAGS_LONG_MODE_CODE);

    for (uint8_t *p = (uint8_t *)&tss; p < (uint8_t *)&tss + sizeof(tss); p++) {
        *p = 0;
    }
    tss.rsp0 = (uint64_t)(uintptr_t)(syscall_stack + sizeof(syscall_stack));
    tss.iomap_base = sizeof(tss_t); /* no I/O permission bitmap -- ring 3 gets no port access */
    tss_set_descriptor((uint64_t)(uintptr_t)&tss, sizeof(tss_t) - 1);

    gdt_ptr_t ptr = {
        .limit = sizeof(gdt_table) - 1,
        .base = (uint64_t)(uintptr_t)&gdt_table,
    };
    load_gdt(&ptr);
    reload_segments();
    load_tr((uint16_t)GDT_TSS_SEL);

    kprintf("gdt: own GDT + TSS loaded (kernel cs=0x%x, tss rsp0=0x%lx)\n",
            GDT_KERNEL_CODE_SEL, tss.rsp0);
}

void tss_set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

void gdt_load_on_this_cpu(void) {
    gdt_ptr_t ptr = {
        .limit = sizeof(gdt_table) - 1,
        .base = (uint64_t)(uintptr_t)&gdt_table,
    };
    load_gdt(&ptr);
    reload_segments();
}
