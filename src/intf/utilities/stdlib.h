#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

int    atoi(const char *s);
long   strtol(const char *s, char **endp, int base);
int    abs(int x);
long   labs(long x);
char  *strdup(const char *s);
void   qsort(void *base, size_t nmemb, size_t size,
             int (*cmp)(const void *, const void *));

#endif
