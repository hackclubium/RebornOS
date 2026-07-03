#include <stdint.h>
#include "lapic.h"

#define IA32_APIC_BASE_MSR 0x1Bu
#define APIC_BASE_ENABLE (1u << 11)
#define APIC_BASE_ADDR_MASK 0xFFFFFF000ULL /* bits 12-35 -- the MMIO base, always 4KiB aligned */

#define LAPIC_REG_ID        0x20u
#define LAPIC_REG_TPR       0x80u /* Task Priority Register -- 0 lets every priority through */
#define LAPIC_REG_SPURIOUS  0xF0u
#define LAPIC_REG_ICR_LOW   0x300u
#define LAPIC_REG_ICR_HIGH  0x310u

#define SPURIOUS_APIC_SOFTWARE_ENABLE (1u << 8)
#define SPURIOUS_VECTOR 0xFFu /* unused vector number, just needs to be >= 32 and not collide with a real IRQ */

#define ICR_DELIVERY_INIT    0x500u /* delivery mode bits 8-10 = 101 (INIT) */
#define ICR_DELIVERY_STARTUP 0x600u /* delivery mode bits 8-10 = 110 (Start-Up) */
#define ICR_LEVEL_ASSERT     (1u << 14)
#define ICR_TRIGGER_LEVEL    (1u << 15)
#define ICR_DELIVERY_PENDING (1u << 12)

static uintptr_t lapic_base;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static uint32_t lapic_read(uint32_t offset) {
    return *(volatile uint32_t *)(lapic_base + offset);
}

static void lapic_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(lapic_base + offset) = value;
}

static void lapic_wait_ipi_idle(void) {
    while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIVERY_PENDING) {
        __asm__ volatile("pause");
    }
}

void lapic_init(void) {
    uint64_t base_msr = rdmsr(IA32_APIC_BASE_MSR);
    lapic_base = (uintptr_t)(base_msr & APIC_BASE_ADDR_MASK);
    wrmsr(IA32_APIC_BASE_MSR, base_msr | APIC_BASE_ENABLE);

    lapic_write(LAPIC_REG_TPR, 0);
    lapic_write(LAPIC_REG_SPURIOUS, SPURIOUS_APIC_SOFTWARE_ENABLE | SPURIOUS_VECTOR);
}

uint32_t lapic_id(void) {
    return lapic_read(LAPIC_REG_ID) >> 24;
}

void lapic_send_init(uint8_t target_apic_id) {
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
    lapic_wait_ipi_idle();
}

void lapic_send_sipi(uint8_t target_apic_id, uint8_t vector) {
    lapic_write(LAPIC_REG_ICR_HIGH, (uint32_t)target_apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, ICR_DELIVERY_STARTUP | vector);
    lapic_wait_ipi_idle();
}
