#include <stdint.h>
#include "lapic.h"
#include "timer.h"
#include "scheduler.h"

#define IA32_APIC_BASE_MSR 0x1Bu
#define APIC_BASE_ENABLE (1u << 11)
#define APIC_BASE_ADDR_MASK 0xFFFFFF000ULL /* bits 12-35 -- the MMIO base, always 4KiB aligned */

#define LAPIC_REG_ID        0x20u
#define LAPIC_REG_EOI       0xB0u
#define LAPIC_REG_TPR       0x80u /* Task Priority Register -- 0 lets every priority through */
#define LAPIC_REG_SPURIOUS  0xF0u
#define LAPIC_REG_ICR_LOW   0x300u
#define LAPIC_REG_ICR_HIGH  0x310u
#define LAPIC_REG_LVT_TIMER 0x320u
#define LAPIC_REG_TIMER_INITIAL_COUNT 0x380u
#define LAPIC_REG_TIMER_CURRENT_COUNT 0x390u
#define LAPIC_REG_TIMER_DIVIDE        0x3E0u

#define LVT_TIMER_PERIODIC (1u << 17)
#define LVT_MASKED         (1u << 16)
#define TIMER_DIVIDE_BY_16 0x3u /* bits 0,1,3 of the divide-config register -- 0b011 = divide by 16 */

/* Measured once by lapic_calibrate_timer(), reused by every core's
 * lapic_timer_start(). */
static uint32_t calibrated_initial_count;

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

void lapic_calibrate_timer(void) {
    lapic_write(LAPIC_REG_TIMER_DIVIDE, TIMER_DIVIDE_BY_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED); /* one-shot, masked -- no interrupt during calibration */

    /* Align to a PIT tick boundary first, so the measurement window
     * below doesn't start mid-tick. */
    uint64_t start_tick = timer_ticks();
    while (timer_ticks() == start_tick) {
        __asm__ volatile("pause");
    }

    lapic_write(LAPIC_REG_TIMER_INITIAL_COUNT, 0xFFFFFFFFu);
    uint64_t calib_start = timer_ticks();
    /* 5 PIT ticks (50ms at timer_init()'s 100Hz) -- long enough for a
     * stable measurement, short enough not to noticeably lengthen
     * boot. */
    while (timer_ticks() < calib_start + 5) {
        __asm__ volatile("pause");
    }
    uint32_t remaining = lapic_read(LAPIC_REG_TIMER_CURRENT_COUNT);
    lapic_write(LAPIC_REG_LVT_TIMER, LVT_MASKED); /* stop it */

    uint32_t elapsed = 0xFFFFFFFFu - remaining;
    /* elapsed ticks happened over 5 PIT ticks -- scale down to what one
     * PIT tick's worth looks like, so every AP's LAPIC timer fires at
     * roughly the same rate the legacy PIT already drives the BSP at. */
    calibrated_initial_count = elapsed / 5u;
}

void lapic_timer_start(void) {
    lapic_write(LAPIC_REG_TIMER_DIVIDE, TIMER_DIVIDE_BY_16);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_VECTOR | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_REG_TIMER_INITIAL_COUNT, calibrated_initial_count);
}

void lapic_timer_isr(interrupt_frame_t *frame) {
    (void)frame;
    /* EOI *before* schedule(), not after: schedule() very often never
     * returns here at all -- it jumps straight into another thread's
     * saved context via switch_context()/switch_context_release(),
     * the same way timer_irq_handler() in timer.c already EOIs before
     * calling its own tick callback. Getting this backwards means the
     * EOI simply never happens on the first real context switch, which
     * leaves this vector permanently "in service" -- the LAPIC then
     * refuses to ever deliver it again, silently freezing preemption on
     * this core for the rest of boot. */
    lapic_write(LAPIC_REG_EOI, 0);
    schedule();
}
