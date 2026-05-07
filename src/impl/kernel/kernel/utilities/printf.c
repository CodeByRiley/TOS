#include "utilities/printf.h"
#include "utilities/string.h"
#include "devices/serial.h"
#include <stdint.h>

struct fmt_ctx {
    char *p;        // current write pos (NULL if no buffer)
    char *end;     // last writable slot (reserved before null terminator)
    int   total;   // total characters that would be written
};

static void fmt_putc(struct fmt_ctx *c, char ch) {
    if (c->p && c->p < c->end) *c->p++ = ch;
    c->total++;
}

static void fmt_puts(struct fmt_ctx *c, const char *s) {
    while (*s) fmt_putc(c, *s++);
}

static void fmt_uint(struct fmt_ctx *c, uint64_t v, int base, int upper,
                     int width, char pad) {
    char tmp[32];
    int  len = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[len++] = '0';
    while (v) {
        tmp[len++] = digits[v % base];
        v /= base;
    }
    while (len < width) { fmt_putc(c, pad); width--; }
    while (len--) fmt_putc(c, tmp[len]);
}

static void fmt_int(struct fmt_ctx *c, int64_t v, int width, char pad) {
    if (v < 0) {
        fmt_putc(c, '-');
        v = -v;
        if (width > 0) width--;
    }
    fmt_uint(c, (uint64_t)v, 10, 0, width, pad);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct fmt_ctx c;
    if (buf && size > 0) {
        c.p   = buf;
        c.end = buf + size - 1;     // reserve last slot for null
    } else {
        c.p = c.end = 0;
    }
    c.total = 0;

    while (*fmt) {
        if (*fmt != '%') { fmt_putc(&c, *fmt++); continue; }
        fmt++;

        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int longflag = 0;
        while (*fmt == 'l') { longflag++; fmt++; }
        if (*fmt == 'z') { longflag = 2; fmt++; }

        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t v = (longflag >= 1)
                    ? (int64_t)va_arg(ap, long)
                    : (int64_t)va_arg(ap, int);
                fmt_int(&c, v, width, pad);
                break;
            }
            case 'u': {
                uint64_t v = (longflag >= 1)
                    ? (uint64_t)va_arg(ap, unsigned long)
                    : (uint64_t)va_arg(ap, unsigned int);
                fmt_uint(&c, v, 10, 0, width, pad);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = (longflag >= 1)
                    ? (uint64_t)va_arg(ap, unsigned long)
                    : (uint64_t)va_arg(ap, unsigned int);
                fmt_uint(&c, v, 16, *fmt == 'X', width, pad);
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)va_arg(ap, void*);
                fmt_puts(&c, "0x");
                fmt_uint(&c, v, 16, 0, 16, '0');
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char*);
                fmt_puts(&c, s ? s : "(null)");
                break;
            }
            case 'c':
                fmt_putc(&c, (char)va_arg(ap, int));
                break;
            case '%':
                fmt_putc(&c, '%');
                break;
            default:
                fmt_putc(&c, *fmt);
        }
        fmt++;
    }

    if (buf && size > 0) *c.p = 0;
    return c.total;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write_str(buf);
    return r;
}
