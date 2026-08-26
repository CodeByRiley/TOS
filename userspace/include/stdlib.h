/* userspace/include/stdlib.h , process + allocator surface.
 *
 * Pulls in size_t. Allocator lives in lib/malloc.c; parsers/qsort/etc.
 * live in lib/stdlib_extra.c.
 */
#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

/* --- Allocation -------------------------------------------------------- */
void  *malloc(size_t n);
void   free(void *p);
void  *calloc(size_t n, size_t sz);
void  *realloc(void *p, size_t n);

/* --- Number parsing ---------------------------------------------------- */
int    atoi(const char *s);
double atof(const char *s);
long   strtol(const char *s, char **endp, int base);

/* --- Process termination ---------------------------------------------- */
void   exit(int code) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));

/* --- Environment (currently always returns NULL / -1) ----------------- */
char  *getenv(const char *name);
int    system(const char *cmd);

/* --- Math helpers + qsort --------------------------------------------- */
int    abs(int x);
long   labs(long x);
void   qsort(void *base, size_t nmemb, size_t size,
             int (*cmp)(const void*, const void*));

/* --- PRNG ------------------------------------------------------------- */
#define RAND_MAX 32767
int    rand(void);
void   srand(unsigned seed);

#endif
