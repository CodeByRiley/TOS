#include "syscall.h"
#include "../include/stdlib.h"

int abs(int x)   { return x < 0 ? -x : x; }
long labs(long x){ return x < 0 ? -x : x; }

int atoi(const char *s) {
    while (*s == ' '||*s=='\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v*10 + (*s - '0'); s++; }
    return v * sign;
}

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

void abort(void) { exit(1); }
char *getenv(const char *n) { (void)n; return 0; }
int   system(const char *cmd) { (void)cmd; return -1; }   /* no shell */

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

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    void *q = malloc(n);
    if (q) {
        // copy old contents (we don't track size, copy n bytes)
        unsigned char *a = (unsigned char*)p;
        unsigned char *b = (unsigned char*)q;
        for (size_t i = 0; i < n; i++) b[i] = a[i];
    }
    free(p);
    return q;
}

static unsigned long rand_state = 1;
int rand(void) { rand_state = rand_state * 1103515245 + 12345; return (int)(rand_state / 65536) & 32767; }
void srand(unsigned s) { rand_state = s; }

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
