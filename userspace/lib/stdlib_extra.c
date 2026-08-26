/* userspace/lib/stdlib_extra.c , stdlib bits beyond malloc/free.
 *
 * Number parsing (atoi/strtol/atof), abs/labs, abort/getenv/system stubs,
 * the linear-congruential rand(), and a Shellsort-grade qsort. None of it
 * is fast or robust , just enough to bring DOOM and the shell up.
 */
#include <lib/syscall.h>
#include <include/stdlib.h>

/* Integer absolute value. */
int abs(int x)   { return x < 0 ? -x : x; }

/* long absolute value. */
long labs(long x){ return x < 0 ? -x : x; }

/* atoi(3): leading whitespace + optional sign + decimal digits. */
int atoi(const char *s) {
    while (*s == ' '||*s=='\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v*10 + (*s - '0'); s++; }
    return v * sign;
}

/* strtol(3): supports base 0 (auto-detect 0x / 0 prefix) and 2-36. */
long strtol(const char *s, char **end, int base) {
    while (*s==' '||*s=='\t') s++;
    long sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) { base=16; s+=2; }
        else if (s[0]=='0') { base=8; s++; }
        else base = 10;
    }
    long v = 0;
    while (*s) {
        int d;
        if (*s>='0'&&*s<='9') d = *s - '0';
        else if (*s>='a'&&*s<='z') d = *s - 'a' + 10;
        else if (*s>='A'&&*s<='Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v*base + d;
        s++;
    }
    if (end) *end = (char*)s;
    return v * sign;
}

/* Abnormal termination , calls exit(1). No signal raised. */
void abort(void) { exit(1); }

/* No environment yet , always NULL. */
char *getenv(const char *n) { (void)n; return 0; }

/* No shell yet , system() always fails. */
int   system(const char *cmd) { (void)cmd; return -1; }

/* atof(3): simple decimal, no exponent support. */
double atof(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    double v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            v += (*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }
    return v * sign;
}

/* realloc(3) , naive: malloc + bytewise copy + free. Since malloc.c
 * doesn't expose the original block size, we copy `n` bytes which may
 * over-read the old allocation if it shrinks. Callers must size up. */
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    void *q = malloc(n);
    if (q) {
        unsigned char *a = (unsigned char*)p;
        unsigned char *b = (unsigned char*)q;
        for (size_t i = 0; i < n; i++) b[i] = a[i];
    }
    free(p);
    return q;
}

/* glibc-style LCG. Period 2^31; "good enough" for DOOM RNG seeds. */
static unsigned long rand_state = 1;
int rand(void) { rand_state = rand_state * 1103515245 + 12345; return (int)(rand_state / 65536) & 32767; }
void srand(unsigned s) { rand_state = s; }

/* qsort(3) implemented as insertion sort , O(n^2). DOOM only sorts small
 * arrays, so the overhead is fine and the code is tiny. */
void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void*, const void*)) {
    char *arr = (char*)base;
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0; j--) {
            char *a = arr + (j-1)*size, *b = arr + j*size;
            if (cmp(a, b) <= 0) break;
            for (size_t k = 0; k < size; k++) { char t=a[k]; a[k]=b[k]; b[k]=t; }
        }
    }
}
