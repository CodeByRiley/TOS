/* userspace/lib/stdio_extra.c — the rest of stdio: putc/puts/printf wrappers,
 * standard streams, the FILE* glue for the formatted writers, and a tiny
 * sscanf used by DOOM's config parser.
 *
 * Buffering: none. fputc / fputs go straight through write(). fflush is a
 * no-op. Stream FILE structs are static so fileno-equivalent code works.
 */
#include "syscall.h"
#include "../include/stdio.h"
#include "../include/string.h"
#include "../include/stdlib.h"

static FILE _stdout = { 1, 0 };
static FILE _stderr = { 2, 0 };
static FILE _stdin  = { 0, 0 };
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;
FILE *stdin  = &_stdin;

/* Write one character to a stream. Returns the char on success, EOF on
 * short write. */
int fputc(int c, FILE *fp) {
    char ch = (char)c;
    return write(fp->fd, &ch, 1) == 1 ? c : EOF;
}

/* Write a NUL-terminated string to a stream (no trailing newline). */
int fputs(const char *s, FILE *fp) {
    long n = (long)strlen(s);
    return write(fp->fd, s, n) == n ? 0 : EOF;
}

/* putc-equivalent on stdout. */
int putchar(int c) { return fputc(c, stdout); }

/* Write `s` + '\n' to stdout. */
int puts(const char *s) { fputs(s, stdout); return fputc('\n', stdout); }

/* Read one byte. Sets EOF and returns EOF on read error. */
int fgetc(FILE *fp) {
    if (!fp || fp->eof) return EOF;
    unsigned char c;
    if (read(fp->fd, &c, 1) <= 0) { fp->eof = 1; return EOF; }
    return c;
}

/* fgets(3): read up to n-1 bytes or until newline. Returns NULL only if
 * zero bytes were read. */
char *fgets(char *s, int n, FILE *fp) {
    int i;
    for (i = 0; i < n - 1; i++) {
        int c = fgetc(fp);
        if (c == EOF) break;
        s[i] = (char)c;
        if (c == '\n') { i++; break; }
    }
    if (i == 0) return 0;
    s[i] = 0;
    return s;
}

/* sprintf(3): unbounded snprintf with a huge (effectively infinite) size. */
int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return r;
}

/* vsprintf(3) — same as sprintf with a va_list already in hand. */
int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
}

/* fprintf(3): format into a 1 KiB scratch buffer then write to stream.
 * Anything beyond that is silently truncated. */
int fprintf(FILE *fp, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(fp->fd, buf, r);
    return r;
}

/* vprintf to stdout. */
int vprintf(const char *fmt, va_list ap) {
    char buf[1024];
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(1, buf, r);
    return r;
}

/* vfprintf to a specific stream. */
int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    char buf[1024];
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(fp->fd, buf, r);
    return r;
}

/* Filesystem ops not supported yet. */
int remove(const char *p) { (void)p; return -1; }
int rename(const char *a, const char *b) { (void)a; (void)b; return -1; }

/* perror(3) without errno strings — caller's `s`, then literal " error". */
void perror(const char *s) { fprintf(stderr, "%s: error\n", s); }

/* No buffering, so nothing to flush. */
int fflush(FILE *fp) { (void)fp; return 0; }

/* Tiny sscanf supporting %d, %i, %u, %x/%X, %s (with optional `l` length
 * modifier). Enough for DOOM's CFG parser; not general-purpose. */
int sscanf(const char *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int matched = 0;
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            int longflag = 0;
            while (*fmt == 'l') { longflag++; fmt++; }
            if (*fmt == 'x' || *fmt == 'X') {
                while (*s == ' ' || *s == '\t') s++;
                unsigned long v = 0;
                int got = 0;
                while (*s) {
                    int d;
                    if (*s >= '0' && *s <= '9') d = *s - '0';
                    else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
                    else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
                    else break;
                    v = v * 16 + d; s++; got = 1;
                }
                if (got) {
                    if (longflag) *va_arg(ap, unsigned long*) = v;
                    else          *va_arg(ap, unsigned int*) = (unsigned int)v;
                    matched++;
                }
            } else if (*fmt == 'd' || *fmt == 'i') {
                while (*s == ' ' || *s == '\t') s++;
                int sign = 1; long v = 0; int got = 0;
                if (*s == '-') { sign = -1; s++; }
                else if (*s == '+') s++;
                while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; got = 1; }
                if (got) {
                    if (longflag) *va_arg(ap, long*) = v * sign;
                    else          *va_arg(ap, int*) = (int)(v * sign);
                    matched++;
                }
            } else if (*fmt == 'u') {
                while (*s == ' ' || *s == '\t') s++;
                unsigned long v = 0; int got = 0;
                while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; got = 1; }
                if (got) {
                    if (longflag) *va_arg(ap, unsigned long*) = v;
                    else          *va_arg(ap, unsigned int*) = (unsigned int)v;
                    matched++;
                }
            } else if (*fmt == 's') {
                while (*s == ' ' || *s == '\t') s++;
                char *p = va_arg(ap, char*);
                while (*s && *s != ' ' && *s != '\t' && *s != '\n') *p++ = *s++;
                *p = 0;
                matched++;
            }
            fmt++;
        } else if (*fmt == ' ' || *fmt == '\t') {
            while (*s == ' ' || *s == '\t') s++;
            fmt++;
        } else {
            if (*s == *fmt) { s++; fmt++; }
            else break;
        }
    }
    va_end(ap);
    return matched;
}
