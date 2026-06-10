/* userspace/lib/string_extra.c — the rest of the str* family.
 *
 * Copies, concat, reverse-find, substring search, case-insensitive
 * compares, and a stubbed strerror. None of them are particularly fast;
 * they exist so DOOM and the shell link cleanly.
 */
#include "syscall.h"
#include "../include/string.h"

/* strcpy(3) — caller guarantees dst is big enough. */
char *strcpy(char *dst, const char *src) { char *d=dst; while ((*d++=*src++)); return dst; }

/* strncpy(3) — pads dst with NULs up to n. Doesn't guarantee NUL termination. */
char *strncpy(char *dst, const char *src, size_t n) { size_t i=0; for (;i<n&&src[i];i++)dst[i]=src[i]; for(;i<n;i++)dst[i]=0; return dst; }

/* strcat(3) — appends src after dst's NUL. */
char *strcat(char *dst, const char *src) { strcpy(dst+strlen(dst), src); return dst; }

/* strncat(3) — appends up to n bytes; always writes a NUL at dst[len+n]. */
char *strncat(char *dst, const char *src, size_t n) { size_t l=strlen(dst); for (size_t i=0;i<n&&src[i];i++)dst[l+i]=src[i]; dst[l+n]=0; return dst; }

/* strrchr(3) — pointer to last occurrence of c, NULL if absent. */
char *strrchr(const char *s, int c) { const char *r=0; while (*s){ if (*s==c) r=s; s++; } return c==0?(char*)s:(char*)r; }

/* strstr(3) — naive O(n*m) substring search. */
char *strstr(const char *h, const char *n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        const char *a=h, *b=n;
        while (*a && *b && *a==*b) { a++; b++; }
        if (!*b) return (char*)h;
    }
    return 0;
}

/* strcasecmp(3) — ASCII-only case folding (A..Z → a..z). */
int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a>='A'&&*a<='Z')?*a+32:*a;
        char cb = (*b>='A'&&*b<='Z')?*b+32:*b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* strncasecmp(3) — same as strcasecmp but bounded by n. */
int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && *b) {
        char ca = (*a>='A'&&*a<='Z')?*a+32:*a;
        char cb = (*b>='A'&&*b<='Z')?*b+32:*b;
        if (ca != cb) return ca - cb;
        a++; b++; n--;
    }
    return n ? ((int)(unsigned char)*a - (int)(unsigned char)*b) : 0;
}

/* strerror(3) stub — no errno table yet. */
char *strerror(int e) { (void)e; return "error"; }
