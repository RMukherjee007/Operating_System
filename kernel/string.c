#include "string.h"

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    while (n--) {
        *d++ = v;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    while (n--) {
        if (*a != *b) return (int)*a - (int)*b;
        a++;
        b++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    if (!s) return 0;
    while (s[len]) len++;
    return len;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t len = 0;
    if (!s) return 0;
    while (len < maxlen && s[len]) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return (int)(uint8_t)*s1 - (int)(uint8_t)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return (int)(uint8_t)*s1 - (int)(uint8_t)*s2;
}

char *strcpy(char *dst, const char *src) {
    char *orig = dst;
    while ((*dst++ = *src++)) {}
    return orig;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *p = dst + strlen(dst);
    while ((*p++ = *src++)) {}
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0) return (char *)haystack;
        haystack++;
    }
    return NULL;
}

int atoi(const char *s) {
    int sign = 1;
    int val = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val * sign;
}

uint64_t strtoul_hex(const char *s, bool *ok) {
    uint64_t val = 0;
    bool any = false;
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s;
        uint64_t d;
        if (c >= '0' && c <= '9') d = (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint64_t)(c - 'A' + 10);
        else break;
        val = (val << 4) | d;
        any = true;
        s++;
    }
    if (ok) *ok = any && (*s == '\0');
    return val;
}

struct buf_writer {
    char *buf;
    size_t size;
    size_t pos;
};

static void bw_putc(struct buf_writer *w, char c) {
    if (w->size > 0 && w->pos + 1 < w->size) {
        w->buf[w->pos] = c;
    }
    w->pos++;
}

static void bw_puts_padded(struct buf_writer *w, const char *s, int width, char pad, bool left_align) {
    int len = (int)strlen(s);
    int spaces = (width > len) ? (width - len) : 0;
    if (!left_align) {
        for (int i = 0; i < spaces; i++) bw_putc(w, pad);
    }
    for (int i = 0; i < len; i++) bw_putc(w, s[i]);
    if (left_align) {
        for (int i = 0; i < spaces; i++) bw_putc(w, ' ');
    }
}

static void format_uint(uint64_t val, unsigned base, bool upper, char *tmp) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[68];
    int n = 0;
    if (val == 0) {
        tmp[0] = '0';
        tmp[1] = '\0';
        return;
    }
    while (val > 0) {
        rev[n++] = digits[val % base];
        val /= base;
    }
    for (int i = 0; i < n; i++) {
        tmp[i] = rev[n - 1 - i];
    }
    tmp[n] = '\0';
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct buf_writer w = { buf, size, 0 };
    char numbuf[72];

    while (*fmt) {
        if (*fmt != '%') {
            bw_putc(&w, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            bw_putc(&w, '%');
            fmt++;
            continue;
        }

        bool left_align = false;
        char pad = ' ';
        int width = 0;
        bool is_long = false;

        while (*fmt == '-' || *fmt == '0') {
            if (*fmt == '-') left_align = true;
            if (*fmt == '0') pad = '0';
            fmt++;
        }
        if (left_align) pad = ' ';

        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        if (*fmt == 'l') {
            is_long = true;
            fmt++;
            if (*fmt == 'l') fmt++;
        } else if (*fmt == 'z') {
            is_long = true;
            fmt++;
        }

        switch (*fmt) {
            case 'c': {
                char c = (char)va_arg(ap, int);
                char s[2] = { c, '\0' };
                bw_puts_padded(&w, s, width, ' ', left_align);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                bw_puts_padded(&w, s, width, ' ', left_align);
                break;
            }
            case 'd':
            case 'i': {
                int64_t v = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
                bool neg = (v < 0);
                uint64_t uv = neg ? (uint64_t)(-v) : (uint64_t)v;
                int offset = 0;
                if (neg) numbuf[offset++] = '-';
                format_uint(uv, 10, false, numbuf + offset);
                bw_puts_padded(&w, numbuf, width, pad, left_align);
                break;
            }
            case 'u': {
                uint64_t uv = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                format_uint(uv, 10, false, numbuf);
                bw_puts_padded(&w, numbuf, width, pad, left_align);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t uv = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                format_uint(uv, 16, (*fmt == 'X'), numbuf);
                bw_puts_padded(&w, numbuf, width, pad, left_align);
                break;
            }
            case 'p': {
                uint64_t uv = (uint64_t)va_arg(ap, void *);
                numbuf[0] = '0';
                numbuf[1] = 'x';
                format_uint(uv, 16, false, numbuf + 2);
                bw_puts_padded(&w, numbuf, width, pad, left_align);
                break;
            }
            default:
                bw_putc(&w, '%');
                if (*fmt) bw_putc(&w, *fmt);
                break;
        }
        if (*fmt) fmt++;
    }

    if (size > 0) {
        size_t term = (w.pos < size) ? w.pos : (size - 1);
        buf[term] = '\0';
    }
    return (int)w.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
