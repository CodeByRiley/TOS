/* userspace/lib/stdio.c — FILE* layer on top of raw fd syscalls.
 *
 * Each FILE wraps a kernel fd plus an EOF flag. The set is just enough
 * for DOOM's WAD loader and a few small tools: no buffering, no ungetc,
 * no error flag. Add a real I/O cache before pushing this beyond toy use.
 */
#include "syscall.h"
#include <stdint.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200

typedef struct {
    int fd;
    int eof;
} FILE;

extern void *malloc(size_t);
extern void  free(void *);

/* Translate fopen(3) mode string to open(2) flag bits.
 * "a" is accepted but treated as plain write+create (no real append yet). */
static int parse_mode(const char *m) {
    if (!m) return O_RDONLY;
    if (m[0] == 'r' && m[1] != '+') return O_RDONLY;
    if (m[0] == 'w') return O_WRONLY | O_CREAT | O_TRUNC;
    if (m[0] == 'a') return O_WRONLY | O_CREAT;
    if (m[0] == 'r' && m[1] == '+') return O_RDWR;
    return O_RDONLY;
}

/* Open `path` with the requested fopen mode. Returns NULL on failure. */
FILE *fopen(const char *path, const char *mode) {
    int flags = parse_mode(mode);
    long fd = open(path, flags);
    if (fd < 0) return 0;
    FILE *fp = (FILE*)malloc(sizeof(FILE));
    if (!fp) { close(fd); return 0; }
    fp->fd = (int)fd;
    fp->eof = 0;
    return fp;
}

/* Close the underlying fd and free the FILE wrapper. */
int fclose(FILE *fp) {
    if (!fp) return -1;
    close(fp->fd);
    free(fp);
    return 0;
}

/* Read up to n*sz bytes; returns number of FULL items read. Sets EOF on
 * short read or kernel error. */
size_t fread(void *buf, size_t sz, size_t n, FILE *fp) {
    if (!fp) return 0;
    long r = read(fp->fd, buf, sz * n);
    if (r <= 0) { fp->eof = 1; return 0; }
    return (size_t)r / sz;
}

/* Write up to n*sz bytes. Returns number of FULL items written. */
size_t fwrite(const void *buf, size_t sz, size_t n, FILE *fp) {
    if (!fp) return 0;
    long r = write(fp->fd, buf, sz * n);
    if (r <= 0) return 0;
    return (size_t)r / sz;
}

/* Reposition fd offset per `whence` (SEEK_SET / CUR / END). */
int fseek(FILE *fp, long off, int whence) {
    if (!fp) return -1;
    long rc = lseek(fp->fd, off, whence);
    if (rc < 0) return -1;
    fp->eof = 0;
    return 0;
}

/* Current fd offset. */
long ftell(FILE *fp) {
    if (!fp) return -1;
    return lseek(fp->fd, 0, SEEK_CUR);
}

/* Returns non-zero once a read has hit end-of-file. NULL fp counts as EOF. */
int feof(FILE *fp) {
    return fp ? fp->eof : 1;
}
