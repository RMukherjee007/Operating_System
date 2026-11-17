#ifndef FULMEN_GIC_H
#define FULMEN_GIC_H

#include "types.h"

/* ARM GICv2 addresses on QEMU virt */
#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL

#define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER(n) (*(volatile uint32_t *)(GICD_BASE + 0x100 + 4 * (n)))
#define GICD_IPRIORITYR(n) (*(volatile uint32_t *)(GICD_BASE + 0x400 + 4 * (n)))
#define GICD_ITARGETSR(n) (*(volatile uint32_t *)(GICD_BASE + 0x800 + 4 * (n)))

#define GICC_CTLR       (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR        (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR        (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR       (*(volatile uint32_t *)(GICC_BASE + 0x010))

#define IRQ_TIMER       30U
#define IRQ_UART0       33U

void gic_init(void);
void gic_enable_irq(uint32_t irq);
uint32_t gic_ack_irq(void);
void gic_eoi(uint32_t iar);

#endif /* FULMEN_GIC_H */
