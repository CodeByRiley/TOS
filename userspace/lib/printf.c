#include "syscall.h"
#include <stdarg.h>
#include <stdint.h>

struct fmt_ctx {
    char *p;
    char *end;
    int   total;
};

static void putc_ctx(struct fmt_ctx *c, char ch) {
    if (c->p && c->p < c->end) *c->p++ = ch;
    c->total++;
}

static void puts_ctx(struct fmt_ctx *c, const char *s) {
    while (*s) putc_ctx(c, *s++);
}

static void putn_ctx(struct fmt_ctx *c, const char *s, int n) {
    while (n-- > 0 && *s) putc_ctx(c, *s++);
}

static void pad_ctx(struct fmt_ctx *c, char ch, int n) {
    while (n-- > 0) putc_ctx(c, ch);
}

static void uint_ctx(struct fmt_ctx *c, uint64_t v, int base, int upper,
                     int width, char pad, int precision, int left) {
    char tmp[32];
    int  len = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0 && precision != 0) tmp[len++] = '0';
    while (v) { tmp[len++] = digits[v % base]; v /= base; }
    int zeroes = precision > len ? precision - len : 0;
    int total = len + zeroes;
    if (!left) pad_ctx(c, pad, width - total);
    pad_ctx(c, '0', zeroes);
    while (len--) putc_ctx(c, tmp[len]);
    if (left) pad_ctx(c, ' ', width - total);
}

static void int_ctx(struct fmt_ctx *c, int64_t v, int width, char pad,
                    int precision, int left) {
    if (v < 0) { putc_ctx(c, '-'); v = -v; if (width > 0) width--; }
    uint_ctx(c, (uint64_t)v, 10, 0, width, pad, precision, left);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct fmt_ctx c = { buf, buf ? buf + (size ? size-1 : 0) : 0, 0 };
    while (*fmt) {
        if (*fmt != '%') { putc_ctx(&c, *fmt++); continue; }
        fmt++;
        int left = 0;
        char pad = ' ';
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        if (left) pad = ' ';
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt - '0'); fmt++; }
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') { precision = precision*10 + (*fmt - '0'); fmt++; }
            pad = ' ';
        }
        int longflag = 0;
        while (*fmt == 'l') { longflag++; fmt++; }
        switch (*fmt) {
            case 'd': case 'i': int_ctx(&c, longflag ? va_arg(ap, long) : va_arg(ap, int), width, pad, precision, left); break;
            case 'u': uint_ctx(&c, longflag ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 10, 0, width, pad, precision, left); break;
            case 'o': uint_ctx(&c, longflag ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 8, 0, width, pad, precision, left); break;
            case 'x': case 'X': uint_ctx(&c, longflag ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, *fmt=='X', width, pad, precision, left); break;
            case 'p': puts_ctx(&c, "0x"); uint_ctx(&c, (uint64_t)va_arg(ap, void*), 16, 0, 16, '0', -1, 0); break;
            case 's': {
                const char *s = va_arg(ap, const char*);
                int len = 0;
                s = s ? s : "(null)";
                while (s[len] && (precision < 0 || len < precision)) len++;
                if (!left) pad_ctx(&c, ' ', width - len);
                putn_ctx(&c, s, len);
                if (left) pad_ctx(&c, ' ', width - len);
                break;
            }
            case 'c': putc_ctx(&c, (char)va_arg(ap, int)); break;
            case '%': putc_ctx(&c, '%'); break;
            default: putc_ctx(&c, *fmt);
        }
        fmt++;
    }
    if (buf && size > 0) *c.p = 0;
    return c.total;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(1, buf, r);
    return r;
}
