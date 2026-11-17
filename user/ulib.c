#include "ulib.h"

extern uint8_t __user_bss_start[];
extern uint8_t __user_bss_end[];
extern int main(int argc, char **argv);

int64_t u_syscall(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2) {
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x8 __asm__("x8") = num;

    __asm__ volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return (int64_t)x0;
}

__attribute__((section(".text.entry")))
void _start(int argc, char **argv) {
    for (uint8_t *p = __user_bss_start; p < __user_bss_end; p++) {
        *p = 0;
    }
    int ret = main(argc, argv);
    sys_exit(ret);
}

void sys_exit(int code) {
    u_syscall(SYS_EXIT, (uint64_t)(int64_t)code, 0, 0);
    for (;;) {}
}

ssize_t sys_write(int fd, const void *buf, size_t len) {
    return (ssize_t)u_syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, (uint64_t)len);
}

ssize_t sys_read(int fd, void *buf, size_t len) {
    return (ssize_t)u_syscall(SYS_READ, (uint64_t)fd, (uint64_t)buf, (uint64_t)len);
}

void sys_yield(void) {
    u_syscall(SYS_YIELD, 0, 0, 0);
}

void sys_sleep(uint64_t ms) {
    u_syscall(SYS_SLEEP, ms, 0, 0);
}

int sys_getpid(void) {
    return (int)u_syscall(SYS_GETPID, 0, 0, 0);
}

int sys_getel(void) {
    return (int)u_syscall(SYS_GETEL, 0, 0, 0);
}

uint64_t sys_uptime(void) {
    return (uint64_t)u_syscall(SYS_UPTIME, 0, 0, 0);
}

void *sys_sbrk(int64_t inc) {
    int64_t r = u_syscall(SYS_SBRK, (uint64_t)inc, 0, 0);
    return (r == -1) ? NULL : (void *)r;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

int atoi(const char *s) {
    int sign = 1, v = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

int putchar(int c) {
    char ch = (char)c;
    sys_write(1, &ch, 1);
    return c;
}

int puts(const char *s) {
    if (!s) s = "(null)";
    sys_write(1, s, strlen(s));
    putchar('\n');
    return 0;
}

static void u_print_uint(uint64_t v, unsigned base, bool upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[64];
    int n = 0;
    if (v == 0) {
        putchar('0');
        return;
    }
    while (v > 0) {
        buf[n++] = digits[v % base];
        v /= base;
    }
    while (n > 0) {
        putchar(buf[--n]);
    }
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char out[256];
    size_t opos = 0;

    while (*fmt) {
        if (*fmt != '%') {
            out[opos++] = *fmt++;
            if (opos == sizeof(out)) {
                sys_write(1, out, opos);
                opos = 0;
            }
            continue;
        }
        if (opos > 0) {
            sys_write(1, out, opos);
            opos = 0;
        }
        fmt++;
        if (*fmt == '%') {
            putchar('%');
            fmt++;
            continue;
        }
        bool is_long = false;
        if (*fmt == 'l') {
            is_long = true;
            fmt++;
            if (*fmt == 'l') fmt++;
        }
        switch (*fmt) {
            case 'c': putchar(va_arg(ap, int)); break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                sys_write(1, s, strlen(s));
                break;
            }
            case 'd':
            case 'i': {
                int64_t v = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
                if (v < 0) {
                    putchar('-');
                    v = -v;
                }
                u_print_uint((uint64_t)v, 10, false);
                break;
            }
            case 'u': {
                uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                u_print_uint(v, 10, false);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                u_print_uint(v, 16, (*fmt == 'X'));
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)va_arg(ap, void *);
                sys_write(1, "0x", 2);
                u_print_uint(v, 16, false);
                break;
            }
            default:
                putchar('%');
                if (*fmt) putchar(*fmt);
                break;
        }
        if (*fmt) fmt++;
    }
    if (opos > 0) sys_write(1, out, opos);
    va_end(ap);
    return 0;
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    return sys_sbrk((int64_t)ALIGN_UP(size, 16));
}

void free(void *ptr) {
    UNUSED(ptr);
}
