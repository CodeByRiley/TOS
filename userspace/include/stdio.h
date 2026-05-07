#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include "../lib/syscall.h"

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct { int fd; int eof; } FILE;

extern FILE *stdout, *stderr, *stdin;

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *fp);
size_t fread(void *buf, size_t sz, size_t n, FILE *fp);
size_t fwrite(const void *buf, size_t sz, size_t n, FILE *fp);
int    fseek(FILE *fp, long off, int whence);
long   ftell(FILE *fp);
int    feof(FILE *fp);
int    fgetc(FILE *fp);
char  *fgets(char *s, int n, FILE *fp);

int printf(const char *fmt, ...);
int fprintf(FILE *fp, const char *fmt, ...);
int sprintf(char *s, const char *fmt, ...);
int snprintf(char *s, size_t n, const char *fmt, ...);
int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *fp, const char *fmt, va_list ap);
int vsprintf(char *s, const char *fmt, va_list ap);

int    fputc(int c, FILE *fp);
int    fputs(const char *s, FILE *fp);
int    putchar(int c);
int    puts(const char *s);
int    fflush(FILE *fp);
int    sscanf(const char *s, const char *fmt, ...);

void   perror(const char *s);
int    remove(const char *path);
int    rename(const char *old, const char *n);

#endif
