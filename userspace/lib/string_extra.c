#include "syscall.h"
#include "../include/string.h"

char *strcpy(char *dst, const char *src) { char *d=dst; while ((*d++=*src++)); return dst; }
char *strncpy(char *dst, const char *src, size_t n) { size_t i=0; for (;i<n&&src[i];i++)dst[i]=src[i]; for(;i<n;i++)dst[i]=0; return dst; }
char *strcat(char *dst, const char *src) { strcpy(dst+strlen(dst), src); return dst; }
char *strncat(char *dst, const char *src, size_t n) { size_t l=strlen(dst); for (size_t i=0;i<n&&src[i];i++)dst[l+i]=src[i]; dst[l+n]=0; return dst; }
char *strrchr(const char *s, int c) { const char *r=0; while (*s){ if (*s==c) r=s; s++; } return c==0?(char*)s:(char*)r; }
char *strstr(const char *h, const char *n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        const char *a=h, *b=n;
        while (*a && *b && *a==*b) { a++; b++; }
        if (!*b) return (char*)h;
    }
    return 0;
}
int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a>='A'&&*a<='Z')?*a+32:*a;
        char cb = (*b>='A'&&*b<='Z')?*b+32:*b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && *b) {
        char ca = (*a>='A'&&*a<='Z')?*a+32:*a;
        char cb = (*b>='A'&&*b<='Z')?*b+32:*b;
        if (ca != cb) return ca - cb;
        a++; b++; n--;
    }
    return n ? ((int)(unsigned char)*a - (int)(unsigned char)*b) : 0;
}
char *strerror(int e) { (void)e; return "error"; }
