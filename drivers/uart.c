#include "uart.h"
#include "fb.h"
#include "virtio_kbd.h"
#include "gic.h"
#include "irq.h"
#include "string.h"

#define UART_RX_BUF_SIZE 256U

static volatile char rx_buf[UART_RX_BUF_SIZE];
static volatile uint32_t rx_head = 0;
static volatile uint32_t rx_tail = 0;
static bool fb_mirror_enabled = true;
static char *capture_buf = NULL;
static size_t capture_cap = 0;
static size_t *capture_len_ptr = NULL;
static int capture_ansi_state = 0;

void uart_init(void) {
    /* Disable UART during configuration */
    UART_CR = 0x0;
    UART_ICR = 0x7FF;

    /* 115200 baud assuming 24 MHz UARTCLK on QEMU virt */
    UART_IBRD = 13;
    UART_FBRD = 1;

    /* 8 bits, no parity, 1 stop bit, enable FIFOs */
    UART_LCR_H = (3U << 5) | (1U << 4);

    /* Mask interrupts for now; enable TX and RX */
    UART_IMSC = 0x0;
    UART_CR = (1U << 0) | (1U << 8) | (1U << 9);
}

void uart_enable_irq(void) {
    UART_ICR = 0x7FF;
    UART_IMSC = (1U << 4) | (1U << 6); /* RXIM | RTIM */
    gic_enable_irq(IRQ_UART0);
}

void uart_handle_irq(void) {
    while (!(UART_FR & UART_FR_RXFE)) {
        char c = (char)(UART_DR & 0xFF);
        uint32_t next = (rx_head + 1) % UART_RX_BUF_SIZE;
        if (next != rx_tail) {
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
    UART_ICR = 0x7FF;
}

void uart_putc(char c) {
    if (c == '\n') {
        while (UART_FR & UART_FR_TXFF) {}
        UART_DR = '\r';
    }
    while (UART_FR & UART_FR_TXFF) {}
    UART_DR = (uint32_t)(uint8_t)c;

    if (fb_mirror_enabled && fb_is_active()) {
        fb_putc(c);
    }

    if (capture_buf && capture_len_ptr && capture_cap > 1U) {
        if (capture_ansi_state == 1) {
            if (c == '[') {
                capture_ansi_state = 2;
            } else {
                capture_ansi_state = 0;
            }
        } else if (capture_ansi_state == 2) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                capture_ansi_state = 0;
            }
        } else if (c == '\033') {
            capture_ansi_state = 1;
        } else if (c == '\n' || (uint8_t)c >= 0x20U || c == '\t') {
            char ch = (c == '\t') ? ' ' : c;
            size_t len = *capture_len_ptr;
            if (len + 1U >= capture_cap) {
                size_t drop = capture_cap / 2U;
                while (drop < len && capture_buf[drop] != '\n') drop++;
                if (drop < len && capture_buf[drop] == '\n') drop++;
                if (drop == 0 || drop >= len) drop = len / 2U;
                size_t rem = len - drop;
                memmove(capture_buf, capture_buf + drop, rem);
                len = rem;
            }
            capture_buf[len++] = ch;
            capture_buf[len] = '\0';
            *capture_len_ptr = len;
        }
    }
}

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        uart_putc(*s++);
    }
}

int uart_getc_nonblock(void) {
    uint64_t flags = irq_save();
    while (!(UART_FR & UART_FR_RXFE)) {
        char c = (char)(UART_DR & 0xFF);
        uint32_t next = (rx_head + 1) % UART_RX_BUF_SIZE;
        if (next != rx_tail) {
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
    int ch = -1;
    if (rx_head != rx_tail) {
        ch = (int)(uint8_t)rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) % UART_RX_BUF_SIZE;
    } else {
        ch = virtio_kbd_getc_nonblock();
    }
    irq_restore(flags);
    return ch;
}

char uart_getc(void) {
    for (;;) {
        int c = uart_getc_nonblock();
        if (c >= 0) return (char)c;
        __asm__ volatile("wfi");
    }
}

void console_set_fb_mirror(bool enabled) {
    fb_mirror_enabled = enabled;
}

void console_set_capture(char *buf, size_t cap, size_t *len_io) {
    capture_buf = buf;
    capture_cap = cap;
    capture_len_ptr = len_io;
    capture_ansi_state = 0;
}

void kvprintf(const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    uart_puts(buf);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
