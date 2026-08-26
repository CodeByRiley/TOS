/* kernel/utilities/printf.c , kernel vsnprintf / snprintf / printf.
 *
 * Single source for every format-string user in the kernel. Supports the
 * full C99 conversion spec set:
 *   - Flags:      -, +, space, #, 0
 *   - Width:      decimal digits or `*` (negative `*` = left-align)
 *   - Precision:  `.N` or `.*`
 *   - Length:     hh, h, l, ll, z, j, t
 *   - Conversions: d, i, u, o, x, X, p, s, c, %
 *
 * No floating-point conversions. The output context holds either a real
 * buffer (snprintf path) or a NULL pointer for "measure only" usage; the
 * total-bytes counter is updated either way so callers can size buffers.
 * printf() writes to COM1 via serial_write_str.
 */
#include <utilities/printf.h>
#include <utilities/string.h>
#include <devices/serial.h>
#include <stdint.h>
#include <stddef.h>

struct fmt_ctx {
    char *p;        // current write pos (NULL if no buffer)
    char *end;      // last writable slot (reserved before null terminator)
    int   total;    // total characters that would be written
};

static void fmt_putc(struct fmt_ctx *c, char ch) {
    if (c->p && c->p < c->end) *c->p++ = ch;
    c->total++;
}

static void fmt_puts(struct fmt_ctx *c, const char *s) {
    while (*s) fmt_putc(c, *s++);
}

static void fmt_putn(struct fmt_ctx *c, const char *s, int n) {
    while (n-- > 0 && *s) fmt_putc(c, *s++);
}

static void fmt_pad(struct fmt_ctx *c, char ch, int n) {
    while (n-- > 0) fmt_putc(c, ch);
}

enum { FL_LEFT = 1, FL_PLUS = 2, FL_SPACE = 4, FL_ALT = 8, FL_ZERO = 16 };

enum {
    LEN_INT, LEN_HH, LEN_H,
    LEN_L,   LEN_LL,
    LEN_Z,   LEN_J,  LEN_T
};

static void emit_uint(struct fmt_ctx *c, uint64_t v, int base, int upper,
                      int width, int precision, int flags,
                      char sign, const char *altprefix, int altprefixlen) {
    char tmp[32];
    int  len = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0 && precision != 0) tmp[len++] = '0';
    while (v) { tmp[len++] = digits[v % base]; v /= base; }
    int zeroes = precision > len ? precision - len : 0;
    int numlen = len + zeroes;
    int prefixlen = (sign ? 1 : 0) + altprefixlen;
    int totallen = numlen + prefixlen;
    int use_zero_pad = (flags & FL_ZERO) && precision < 0 && !(flags & FL_LEFT);
    if (!(flags & FL_LEFT) && !use_zero_pad)
        fmt_pad(c, ' ', width - totallen);
    if (sign) fmt_putc(c, sign);
    for (int i = 0; i < altprefixlen; i++) fmt_putc(c, altprefix[i]);
    if (use_zero_pad)
        fmt_pad(c, '0', width - totallen);
    fmt_pad(c, '0', zeroes);
    while (len--) fmt_putc(c, tmp[len]);
    if (flags & FL_LEFT)
        fmt_pad(c, ' ', width - totallen);
}

static void emit_int(struct fmt_ctx *c, int64_t v, int width, int precision,
                     int flags) {
    char sign = 0;
    uint64_t u;
    if (v < 0) { sign = '-'; u = (uint64_t)(-v); }
    else {
        u = (uint64_t)v;
        if (flags & FL_PLUS)       sign = '+';
        else if (flags & FL_SPACE) sign = ' ';
    }
    emit_uint(c, u, 10, 0, width, precision, flags, sign, 0, 0);
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

        int flags = 0;
        for (;;) {
            if      (*fmt == '-') { flags |= FL_LEFT;  fmt++; }
            else if (*fmt == '+') { flags |= FL_PLUS;  fmt++; }
            else if (*fmt == ' ') { flags |= FL_SPACE; fmt++; }
            else if (*fmt == '#') { flags |= FL_ALT;   fmt++; }
            else if (*fmt == '0') { flags |= FL_ZERO;  fmt++; }
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
            if (width < 0) { flags |= FL_LEFT; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
                if (precision < 0) precision = -1;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        int lenmod = LEN_INT;
        if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') { lenmod = LEN_HH; fmt++; }
            else             { lenmod = LEN_H; }
        } else if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { lenmod = LEN_LL; fmt++; }
            else             { lenmod = LEN_L; }
        } else if (*fmt == 'z') { lenmod = LEN_Z; fmt++; }
        else if   (*fmt == 'j') { lenmod = LEN_J; fmt++; }
        else if   (*fmt == 't') { lenmod = LEN_T; fmt++; }

        switch (*fmt) {
            case 'd': case 'i': {
                int64_t v;
                switch (lenmod) {
                    case LEN_HH: v = (signed char)va_arg(ap, int); break;
                    case LEN_H:  v = (short)va_arg(ap, int); break;
                    case LEN_L:  v = (int64_t)va_arg(ap, long); break;
                    case LEN_LL: v = (int64_t)va_arg(ap, long long); break;
                    case LEN_Z:  v = (int64_t)va_arg(ap, size_t); break;
                    case LEN_J:  v = (int64_t)va_arg(ap, long long); break;
                    case LEN_T:  v = (int64_t)va_arg(ap, ptrdiff_t); break;
                    default:     v = (int64_t)va_arg(ap, int); break;
                }
                emit_int(&c, v, width, precision, flags);
                break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                uint64_t v;
                switch (lenmod) {
                    case LEN_HH: v = (unsigned char)va_arg(ap, unsigned int); break;
                    case LEN_H:  v = (unsigned short)va_arg(ap, unsigned int); break;
                    case LEN_L:  v = (uint64_t)va_arg(ap, unsigned long); break;
                    case LEN_LL: v = (uint64_t)va_arg(ap, unsigned long long); break;
                    case LEN_Z:  v = (uint64_t)va_arg(ap, size_t); break;
                    case LEN_J:  v = (uint64_t)va_arg(ap, unsigned long long); break;
                    case LEN_T:  v = (uint64_t)va_arg(ap, ptrdiff_t); break;
                    default:     v = (uint64_t)va_arg(ap, unsigned int); break;
                }
                int base  = (*fmt == 'o') ? 8 : (*fmt == 'u') ? 10 : 16;
                int upper = (*fmt == 'X');
                const char *alt = 0; int altlen = 0;
                if ((flags & FL_ALT) && v != 0) {
                    if      (*fmt == 'o') { alt = "0";  altlen = 1; }
                    else if (*fmt == 'x') { alt = "0x"; altlen = 2; }
                    else if (*fmt == 'X') { alt = "0X"; altlen = 2; }
                }
                emit_uint(&c, v, base, upper, width, precision, flags, 0, alt, altlen);
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)va_arg(ap, void*);
                fmt_puts(&c, "0x");
                emit_uint(&c, v, 16, 0, 16, -1, FL_ZERO, 0, 0, 0);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char*);
                int len = 0;
                s = s ? s : "(null)";
                while (s[len] && (precision < 0 || len < precision)) len++;
                if (!(flags & FL_LEFT)) fmt_pad(&c, ' ', width - len);
                fmt_putn(&c, s, len);
                if (flags & FL_LEFT)   fmt_pad(&c, ' ', width - len);
                break;
            }
            case 'c': {
                char ch = (char)va_arg(ap, int);
                if (!(flags & FL_LEFT)) fmt_pad(&c, ' ', width - 1);
                fmt_putc(&c, ch);
                if (flags & FL_LEFT)   fmt_pad(&c, ' ', width - 1);
                break;
            }
            case '%': fmt_putc(&c, '%'); break;
            default:  fmt_putc(&c, *fmt); break;
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
