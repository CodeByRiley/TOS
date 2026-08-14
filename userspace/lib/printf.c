/* userspace/lib/printf.c — vsnprintf / snprintf / printf.
 *
 * Single source for every format-string user in userspace. Supports the
 * full C99 conversion spec set:
 *   - Flags:      -, +, space, #, 0
 *   - Width:      decimal digits or `*` (negative `*` = left-align)
 *   - Precision:  `.N` or `.*`
 *   - Length:     hh, h, l, ll, z, j, t
 *   - Conversions: d, i, u, o, x, X, p, s, c, %
 * No floating-point conversions. The output context holds either a real
 * buffer (snprintf path) or a NULL pointer for "measure only" usage; the
 * total-bytes counter is updated either way so callers can size buffers.
 */
#include <syscall.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

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
        pad_ctx(c, ' ', width - totallen);
    if (sign) putc_ctx(c, sign);
    for (int i = 0; i < altprefixlen; i++) putc_ctx(c, altprefix[i]);
    if (use_zero_pad)
        pad_ctx(c, '0', width - totallen);
    pad_ctx(c, '0', zeroes);
    while (len--) putc_ctx(c, tmp[len]);
    if (flags & FL_LEFT)
        pad_ctx(c, ' ', width - totallen);
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
    struct fmt_ctx c = { buf, buf ? buf + (size ? size - 1 : 0) : 0, 0 };
    while (*fmt) {
        if (*fmt != '%') { putc_ctx(&c, *fmt++); continue; }
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
                puts_ctx(&c, "0x");
                emit_uint(&c, v, 16, 0, 16, -1, FL_ZERO, 0, 0, 0);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char*);
                int len = 0;
                s = s ? s : "(null)";
                while (s[len] && (precision < 0 || len < precision)) len++;
                if (!(flags & FL_LEFT)) pad_ctx(&c, ' ', width - len);
                putn_ctx(&c, s, len);
                if (flags & FL_LEFT)   pad_ctx(&c, ' ', width - len);
                break;
            }
            case 'c': {
                char ch = (char)va_arg(ap, int);
                if (!(flags & FL_LEFT)) pad_ctx(&c, ' ', width - 1);
                putc_ctx(&c, ch);
                if (flags & FL_LEFT)   pad_ctx(&c, ' ', width - 1);
                break;
            }
            case '%': putc_ctx(&c, '%'); break;
            default:  putc_ctx(&c, *fmt); break;
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
    if (r > (int)sizeof(buf)) r = sizeof(buf) - 1;
    va_end(ap);
    write(1, buf, r);
    return r;
}
