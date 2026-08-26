/* userspace/include/string.h , mem... / str... prototypes.
 *
 * Implementations split across lib/string.c (core) and
 * lib/string_extra.c (the rest of the str* family + strerror stub).
 */
#ifndef STRING_H
#define STRING_H
#include <stddef.h>

void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
void   *memset(void *dst, int c, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
size_t  strlen(const char *s);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strcat(char *dst, const char *src);
char   *strncat(char *dst, const char *src, size_t n);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
int     strcasecmp(const char *a, const char *b);
int     strncasecmp(const char *a, const char *b, size_t n);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr(const char *h, const char *n);
char   *strdup(const char *s);
char   *strerror(int e);

#endif
