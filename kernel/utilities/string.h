/* kernel/utilities/string.h , kernel mem... / str... surface.
 *
 * Implementations live in kernel/utilities/string.c. Mirrors the
 * userspace string.h API so code (e.g., the formatter) can be shared
 * cleanly across kernel + userspace.
 */
#ifndef STRING_H
#define STRING_H

#include <utilities/types.h>
#include <stddef.h>
#include <stdint.h>

void   *memcpy(void *dst, const void *src, usize n);
void   *memmove(void *dst, const void *src, usize n);
void   *memset(void *dst, int c, usize n);
int     memcmp(const void *a, const void *b, usize n);

usize  strlen(const char *s);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, usize n);
char   *strcat(char *dst, const char *src);
char   *strncat(char *dst, const char *src, usize n);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, usize n);
int     strcasecmp(const char *a, const char *b);
int     strncasecmp(const char *a, const char *b, usize n);
char   *strlwr(const char *s);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr(const char *h, const char *n);
char   *strdup(const char *s);
char   *strerror(int e);

#endif
