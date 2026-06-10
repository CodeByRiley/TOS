/* src/intf/fs/stdio.h — kernel-internal stdio over FAT.
 *
 * FILE wraps a fat_file and an in-use bit. Used by the kernel logger and
 * any code that wants buffered file access; userspace gets its own
 * implementation via syscalls.
 *
 * Implementation: src/impl/kernel/fs/stdio.c.
 */
#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include "fs/fat.h"

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct {
    struct fat_file f;
    int             valid;
} FILE;

FILE  *fopen(const char *name, const char *mode);
int    fclose(FILE *fp);
size_t fread(void *buf, size_t sz, size_t nmemb, FILE *fp);
int    fseek(FILE *fp, long off, int whence);
long   ftell(FILE *fp);
int    fgetc(FILE *fp);
int    feof(FILE *fp);

#endif
