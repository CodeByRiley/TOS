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

int fputc(int c, FILE *fp) {
    char ch = (char)c;
    return write(fp->fd, &ch, 1) == 1 ? c : EOF;
}

int fputs(const char *s, FILE *fp) {
    long n = (long)strlen(s);
    return write(fp->fd, s, n) == n ? 0 : EOF;
}

int putchar(int c) { return fputc(c, stdout); }
int puts(const char *s) { fputs(s, stdout); return fputc('\n', stdout); }

int fgetc(FILE *fp) {
    if (!fp || fp->eof) return EOF;
    unsigned char c;
    if (read(fp->fd, &c, 1) <= 0) { fp->eof = 1; return EOF; }
    return c;
}

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

/* fwrite defined in stdio.c */

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
}

int fprintf(FILE *fp, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(fp->fd, buf, r);
    return r;
}

int vprintf(const char *fmt, va_list ap) {
    char buf[1024];
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(1, buf, r);
    return r;
}

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    char buf[1024];
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(fp->fd, buf, r);
    return r;
}

int remove(const char *p) { (void)p; return -1; }
int rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
void perror(const char *s) { fprintf(stderr, "%s: error\n", s); }

int fflush(FILE *fp) { (void)fp; return 0; }   /* no buffering, no-op */

/* minimal sscanf: supports %d, %x/%X, %s, %u — enough for DOOM config parsing */
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
