#ifndef FULMEN_IRQ_H
#define FULMEN_IRQ_H

#include "types.h"

/* Saved CPU trap frame (must match kernel_entry/kernel_exit in boot/vectors.S: 272 bytes) */
struct pt_regs {
    uint64_t regs[31];
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
};

static inline void irq_enable(void) {
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline void irq_disable(void) {
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile(
        "mrs %0, daif\n"
        "msr daifset, #2\n"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}

void sync_exception_handler(struct pt_regs *regs, uint64_t esr, uint64_t far, uint64_t from_el0);
void irq_handler(struct pt_regs *regs);
void bad_exception_handler(uint64_t type, uint64_t esr, uint64_t elr, uint64_t far);

#endif /* FULMEN_IRQ_H */
