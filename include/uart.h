#ifndef FULMEN_UART_H
#define FULMEN_UART_H

#include "types.h"

/* PL011 UART0 base on QEMU virt */
#define UART0_BASE   0x09000000UL

#define UART_DR      (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART_FR      (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define UART_IBRD    (*(volatile uint32_t *)(UART0_BASE + 0x24))
#define UART_FBRD    (*(volatile uint32_t *)(UART0_BASE + 0x28))
#define UART_LCR_H   (*(volatile uint32_t *)(UART0_BASE + 0x2C))
#define UART_CR      (*(volatile uint32_t *)(UART0_BASE + 0x30))
#define UART_IMSC    (*(volatile uint32_t *)(UART0_BASE + 0x38))
#define UART_MIS     (*(volatile uint32_t *)(UART0_BASE + 0x40))
#define UART_ICR     (*(volatile uint32_t *)(UART0_BASE + 0x44))

#define UART_FR_RXFE (1U << 4)
#define UART_FR_TXFF (1U << 5)

/* ANSI color escapes */
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_WHITE   "\033[37m"

void uart_init(void);
void uart_enable_irq(void);
void uart_handle_irq(void);
void uart_putc(char c);
void uart_puts(const char *s);
char uart_getc(void);
int uart_getc_nonblock(void);

void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);
void console_set_fb_mirror(bool enabled);
void console_set_capture(char *buf, size_t cap, size_t *len_io);

#endif /* FULMEN_UART_H */
