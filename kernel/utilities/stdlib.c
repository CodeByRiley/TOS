/* kernel/utilities/stdlib.c , kernel stdlib subset.
 *
 * atoi / strtol number parsers, abs / labs, and a Shellsort-grade qsort.
 * strdup lives in string.c so it sits next to the rest of the str* code.
 */
#include <utilities/stdlib.h>
#include <utilities/string.h>
#include <memory/heap.h>
#include <stdint.h>

/* atoi(3): leading whitespace + optional sign + decimal digits. */
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

/* strtol(3): base 0 auto-detects 0x / 0 prefixes; base 16 accepts an
 * optional 0x prefix. Stops at the first non-digit and writes the rest
 * to *endp if non-NULL. */
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

/* qsort(3) implemented as insertion sort. O(n^2), correct, no extra
 * memory. DOOM only sorts small arrays so it's fine until profiling
 * says otherwise. */
void qsort(void *base, usize nmemb, usize size,
           int (*cmp)(const void *, const void *)) {
    u8 *arr = (u8*)base;
    for (usize i = 1; i < nmemb; i++) {
        for (usize j = i; j > 0; j--) {
            u8 *a = arr + (j - 1) * size;
            u8 *b = arr + j * size;
            if (cmp(a, b) <= 0) break;
            for (usize k = 0; k < size; k++) {
                u8 t = a[k];
                a[k] = b[k];
                b[k] = t;
            }
        }
    }
}
