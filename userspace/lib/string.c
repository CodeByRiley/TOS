/* userspace/lib/string.c — core mem... / str... primitives.
 *
 * mem* routines fast-path the 8-byte-aligned case (common for our heap +
 * structs) and fall through to byte copies for the tail. str* routines
 * are straightforward byte loops. Nothing here uses SIMD or libc tricks.
 */
#include "../include/string.h"
#include <stdint.h>

/* Copy n bytes from src to dst. dst and src must not overlap. */
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
        uint64_t       *D = (uint64_t*)d;
        const uint64_t *S = (const uint64_t*)s;
        size_t q = n >> 3;
        while (q--) *D++ = *S++;
        d = (uint8_t*)D;
        s = (const uint8_t*)S;
        n &= 7;
    }
    while (n--) *d++ = *s++;
    return dst;
}

/* Fill n bytes of dst with byte value c. */
void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    if (((uintptr_t)d & 7) == 0 && n >= 8) {
        uint64_t v = (uint8_t)c;
        v |= v << 8;
        v |= v << 16;
        v |= v << 32;
        uint64_t *D = (uint64_t*)d;
        size_t q = n >> 3;
        while (q--) *D++ = v;
        d = (uint8_t*)D;
        n &= 7;
    }
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

/* Overlap-safe memcpy. Copies forward when dst < src, backward otherwise. */
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(dst, src, n);
    d += n; s += n;
    while (n--) *--d = *--s;
    return dst;
}

/* memcmp(3): byte-wise compare, returns *a-*b at first mismatch. */
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t*)a;
    const uint8_t *y = (const uint8_t*)b;
    if ((((uintptr_t)x | (uintptr_t)y) & 7) == 0) {
        const uint64_t *X = (const uint64_t*)x;
        const uint64_t *Y = (const uint64_t*)y;
        while (n >= 8) {
            if (*X != *Y) break;
            X++; Y++;
            n -= 8;
        }
        x = (const uint8_t*)X;
        y = (const uint8_t*)Y;
    }
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

/* strlen(3). */
size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

/* strcmp(3). */
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

/* strncmp(3) — stops at min(n, strlen(a), strlen(b)). */
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n == 0 ? 0 : (uint8_t)*a - (uint8_t)*b;
}

/* strchr(3). Returns pointer to the terminator if c==0, NULL if not found. */
char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return c == 0 ? (char*)s : 0;
}

/* strdup(3) — caller frees. NULL on OOM. */
char *strdup(const char *s) {
    extern void *malloc(size_t);
    size_t len = strlen(s) + 1;
    char *p = (char*)malloc(len);
    if (!p) return 0;
    memcpy(p, s, len);
    return p;
}
