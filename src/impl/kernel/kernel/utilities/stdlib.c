#include "utilities/stdlib.h"
#include "utilities/string.h"
#include "memory/heap.h"
#include <stdint.h>

int atoi(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

long strtol(const char *s, char **endp, int base) {
    while (*s == ' ' || *s == '\t') s++;
    long sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0')                            { base = 8;  s += 1; }
        else                                              { base = 10;          }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    long v = 0;
    while (*s) {
        int d;
        if      (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (endp) *endp = (char*)s;
    return v * sign;
}

int  abs(int x)   { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = (char*)kmalloc(len);
    if (!p) return 0;
    memcpy(p, s, len);
    return p;
}

// insertion sort: O(n²), correct, no extra memory. Fine until DOOM profiles bad.
void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *)) {
    uint8_t *arr = (uint8_t*)base;
    for (size_t i = 1; i < nmemb; i++) {
        for (size_t j = i; j > 0; j--) {
            uint8_t *a = arr + (j - 1) * size;
            uint8_t *b = arr + j * size;
            if (cmp(a, b) <= 0) break;
            for (size_t k = 0; k < size; k++) {
                uint8_t t = a[k];
                a[k] = b[k];
                b[k] = t;
            }
        }
    }
}
