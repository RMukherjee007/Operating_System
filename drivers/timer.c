#include "timer.h"
#include "gic.h"
#include "fb.h"
#include "sched.h"

static volatile uint64_t system_ticks = 0;
static uint64_t cntfrq_hz = 62500000UL;
static uint64_t ticks_per_interval = 625000UL;

static inline uint64_t read_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_tval(uint64_t v) {
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_cntp_ctl(uint64_t v) {
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
}

void timer_init(void) {
    uint64_t freq = read_cntfrq();
    if (freq == 0) freq = 62500000UL;
    cntfrq_hz = freq;
    ticks_per_interval = freq / TIMER_HZ;

    write_cntp_tval(ticks_per_interval);
    write_cntp_ctl(1UL); /* ENABLE=1, IMASK=0 */
    gic_enable_irq(IRQ_TIMER);
}

void timer_handle_irq(void) {
    /* Reload physical timer for next 10ms tick (clears ISTATUS) */
    write_cntp_tval(ticks_per_interval);
    system_ticks++;

    /* Refresh graphical status HUD every 250ms */
    if ((system_ticks % 25U) == 0 && fb_is_active()) {
        fb_update_hud();
    }

    scheduler_tick();
}

uint64_t timer_get_ticks(void) {
    return system_ticks;
}

uint64_t timer_get_ms(void) {
    return system_ticks * (1000UL / TIMER_HZ);
}

uint64_t timer_get_freq(void) {
    return cntfrq_hz;
}
