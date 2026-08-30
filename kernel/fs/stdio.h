/* kernel/fs/stdio.h , kernel-internal stdio over the VFS.
 *
 * FILE wraps a vfs_file, an in-use bit, and the access rights its mode
 * string granted. Used by the kernel logger, the ELF and PE loaders, and
 * any code that wants file access; userspace gets its own implementation
 * via syscalls.
 *
 * fread/fwrite return a count of complete items, as in C , not bytes.
 *
 * Implementation: kernel/fs/stdio.c.
 */
#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>
#include <fs/vfs/vfs.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct {
    struct vfs_file f;
    int             valid;
    int             can_read;  /* Opened with 'r' or '+'. */
    int             can_write; /* Opened with 'w', 'a', or '+'. */
    int             append;    /* Writes always seek to EOF. */
} FILE;

FILE  *fopen(const char *name, const char *mode);
int    fclose(FILE *fp);
size_t fread(void *buf, size_t sz, size_t nmemb, FILE *fp);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *fp);
int    fseek(FILE *fp, long off, int whence);
long   ftell(FILE *fp);
int    fgetc(FILE *fp);
int    feof(FILE *fp);

#endif
