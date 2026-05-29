#include "utilities/string.h"
#include "memory/heap.h"

void *memcpy(void *dst, const void *src, size_t n) {
    void *ret = dst;
    size_t q = n >> 3;
    size_t b = n & 7;
    __asm__ volatile ("cld; rep movsq"
                      : "+D"(dst), "+S"(src), "+c"(q)
                      :: "memory");
    __asm__ volatile ("rep movsb"
                      : "+D"(dst), "+S"(src), "+c"(b)
                      :: "memory");
    return ret;
}

void *memset(void *dst, int c, size_t n) {
    void *ret = dst;
    uint64_t v = (uint8_t)c;
    v |= v << 8;
    v |= v << 16;
    v |= v << 32;
    size_t q = n >> 3;
    size_t b = n & 7;
    __asm__ volatile ("cld; rep stosq"
                      : "+D"(dst), "+c"(q)
                      : "a"(v)
                      : "memory");
    __asm__ volatile ("rep stosb"
                      : "+D"(dst), "+c"(b)
                      : "a"(v)
                      : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(dst, src, n);
    /* Overlap with dst > src: copy backward, byte stride. Rare path
     * (console scroll). Speed not critical here. */
    d += n; s += n;
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    while (n--) {
        if (*x != *y) return *x - *y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++)           dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while (*src) *d++ = *src++;
    *d = 0;
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = 0;
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n == 0 ? 0 : (uint8_t)*a - (uint8_t)*b;
}

static char to_lower_byte(char c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = to_lower_byte(*a), cb = to_lower_byte(*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && *b) {
        char ca = to_lower_byte(*a), cb = to_lower_byte(*b);
        if (ca != cb) return ca - cb;
        a++; b++; n--;
    }
    return n == 0 ? 0 : (uint8_t)*a - (uint8_t)*b;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return c == 0 ? (char *)s : 0;
}

char *strrchr(const char *s, int c) {
    const char *last = 0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == 0) return (char *)s;
    return (char *)last;
}

char *strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h;
        const char *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return 0;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = (char *)kmalloc(len);
    if (!p) return 0;
    memcpy(p, s, len);
    return p;
}

char *strerror(int e) {
    (void)e;
    return "error";
}
