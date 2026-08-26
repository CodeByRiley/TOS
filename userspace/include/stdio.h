/* userspace/include/stdio.h , FILE* + formatted I/O surface.
 *
 * FILE is an opaque struct {fd, eof}; no buffering. Implementations:
 *   - lib/stdio.c        : fopen/fclose/fread/fwrite/fseek/ftell/feof/fgetc/fgets
 *   - lib/stdio_extra.c  : printf-family, fputc/puts, perror, sscanf,
 *                          remove/rename stubs, standard-stream FILE objects
 *   - lib/printf.c       : the actual vsnprintf format engine
 */
#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <lib/syscall.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct { int fd; int eof; } FILE;

/* Standard streams. Backed by fd 0/1/2 in lib/stdio_extra.c. */
extern FILE *stdout, *stderr, *stdin;

/* --- FILE* lifecycle + raw I/O ---------------------------------------- */
FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *fp);
size_t fread(void *buf, size_t sz, size_t n, FILE *fp);
size_t fwrite(const void *buf, size_t sz, size_t n, FILE *fp);
int    fseek(FILE *fp, long off, int whence);
long   ftell(FILE *fp);
int    feof(FILE *fp);
int    fgetc(FILE *fp);
char  *fgets(char *s, int n, FILE *fp);

/* --- Formatted output -------------------------------------------------- */
int printf(const char *fmt, ...);
int fprintf(FILE *fp, const char *fmt, ...);
int sprintf(char *s, const char *fmt, ...);
int snprintf(char *s, size_t n, const char *fmt, ...);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *fp, const char *fmt, va_list ap);
int vsprintf(char *s, const char *fmt, va_list ap);

/* --- Character + line I/O --------------------------------------------- */
int    fputc(int c, FILE *fp);
int    fputs(const char *s, FILE *fp);
int    putchar(int c);
int    puts(const char *s);
int    fflush(FILE *fp);

/* --- Tiny scanf (lib/stdio_extra.c) ----------------------------------- */
int    sscanf(const char *s, const char *fmt, ...);

/* --- Misc ------------------------------------------------------------- */
void   perror(const char *s);
int    remove(const char *path);
int    rename(const char *old, const char *n);

#endif
