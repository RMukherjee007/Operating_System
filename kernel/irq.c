#include "irq.h"
#include "gic.h"
#include "timer.h"
#include "uart.h"
#include "sched.h"
#include "syscall.h"

static const char *bad_type_names[] = {
    "SYNC_EL1t", "IRQ_EL1t", "FIQ_EL1t", "SERROR_EL1t",
    "SYNC_EL1h", "IRQ_EL1h", "FIQ_EL1h", "SERROR_EL1h",
    "SYNC_EL0_64", "IRQ_EL0_64", "FIQ_EL0_64", "SERROR_EL0_64",
    "SYNC_EL0_32", "IRQ_EL0_32", "FIQ_EL0_32", "SERROR_EL0_32"
};

void sync_exception_handler(struct pt_regs *regs, uint64_t esr, uint64_t far, uint64_t from_el0) {
    uint32_t ec = (uint32_t)((esr >> 26) & 0x3FUL);

    if (from_el0 && ec == 0x15U) {
        /* SVC from AArch64 EL0 user program */
        syscall_handler(regs);
        return;
    }

    if (from_el0) {
        kprintf(ANSI_RED "\n[EL0 USER FAULT] EC=0x%02x ESR=0x%016lx FAR=0x%016lx ELR=0x%016lx\n" ANSI_RESET,
                ec, esr, far, regs->elr_el1);
        kprintf("Terminating faulting EL0 program cleanly.\n");
        exit_el0(-1);
        return;
    }

    kprintf(ANSI_RED "\n[KERNEL PANIC] Synchronous Exception in EL1!\n" ANSI_RESET);
    kprintf("  EC=0x%02x ESR=0x%016lx ELR=0x%016lx FAR=0x%016lx\n",
            ec, esr, regs->elr_el1, far);
    for (;;) __asm__ volatile("wfe");
}

void irq_handler(struct pt_regs *regs) {
    UNUSED(regs);
    uint32_t iar = gic_ack_irq();
    uint32_t irq = iar & 0x3FFU;

    if (irq == 1023U) {
        /* Spurious interrupt */
        return;
    }

    if (irq == IRQ_TIMER) {
        timer_handle_irq();
    } else if (irq == IRQ_UART0) {
        uart_handle_irq();
    }

    gic_eoi(iar);

    if (irq == IRQ_TIMER && sched_should_preempt()) {
        schedule();
    }
}

void bad_exception_handler(uint64_t type, uint64_t esr, uint64_t elr, uint64_t far) {
    const char *tname = (type < 16U) ? bad_type_names[type] : "UNKNOWN";
    kprintf(ANSI_RED "\n[FATAL EXCEPTION: %s] ESR=0x%016lx ELR=0x%016lx FAR=0x%016lx\n" ANSI_RESET,
            tname, esr, elr, far);
    for (;;) __asm__ volatile("wfe");
}
