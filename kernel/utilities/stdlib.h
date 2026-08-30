/* kernel/utilities/stdlib.h , kernel-side stdlib subset.
 *
 * Number parsers + abs + strdup + qsort. Implementations live in
 * kernel/utilities/stdlib.c. Memory-allocation helpers (kmalloc/
 * kfree) live in memory/heap.h, not here.
 */
#ifndef STDLIB_H
#define STDLIB_H

#include <utilities/types.h>
#include <stddef.h>

int    atoi(const char *s);
long   strtol(const char *s, char **endp, int base);
int    abs(int x);
long   labs(long x);
char  *strdup(const char *s);
void   qsort(void *base, usize nmemb, usize size,
             int (*cmp)(const void *, const void *));

#endif
