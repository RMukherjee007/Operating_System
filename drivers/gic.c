#include "gic.h"

void gic_init(void) {
    /* Disable distributor while configuring */
    GICD_CTLR = 0x0;

    /* Route SPIs (IRQ >= 32) to CPU 0 */
    for (uint32_t i = 8; i < 64; i++) {
        GICD_ITARGETSR(i) = 0x01010101U;
    }

    /* Set default priority 0xA0 for all IRQs */
    for (uint32_t i = 0; i < 64; i++) {
        GICD_IPRIORITYR(i) = 0xA0A0A0A0U;
    }

    /* Enable GIC Distributor */
    GICD_CTLR = 0x1;

    /* Set CPU interface priority mask to allow all priorities (< 0xFF) */
    GICC_PMR = 0xF0;
    GICC_CTLR = 0x1;
}

void gic_enable_irq(uint32_t irq) {
    uint32_t reg = irq / 32U;
    uint32_t bit = irq % 32U;
    GICD_ISENABLER(reg) = (1U << bit);
}

uint32_t gic_ack_irq(void) {
    return GICC_IAR;
}

void gic_eoi(uint32_t iar) {
    GICC_EOIR = iar;
}
