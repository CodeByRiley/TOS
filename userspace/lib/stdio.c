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

static int parse_mode(const char *m) {
    if (!m) return O_RDONLY;
    /* "r" -> read-only; "w" -> write+create+truncate; "rb"/"wb" same; "a" -> append (treated as write for now) */
    if (m[0] == 'r' && m[1] != '+') return O_RDONLY;
    if (m[0] == 'w') return O_WRONLY | O_CREAT | O_TRUNC;
    if (m[0] == 'a') return O_WRONLY | O_CREAT;
    if (m[0] == 'r' && m[1] == '+') return O_RDWR;
    return O_RDONLY;
}

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

int fclose(FILE *fp) {
    if (!fp) return -1;
    close(fp->fd);
    free(fp);
    return 0;
}

size_t fread(void *buf, size_t sz, size_t n, FILE *fp) {
    if (!fp) return 0;
    long r = read(fp->fd, buf, sz * n);
    if (r <= 0) { fp->eof = 1; return 0; }
    return (size_t)r / sz;
}

size_t fwrite(const void *buf, size_t sz, size_t n, FILE *fp) {
    if (!fp) return 0;
    long r = write(fp->fd, buf, sz * n);
    if (r <= 0) return 0;
    return (size_t)r / sz;
}

int fseek(FILE *fp, long off, int whence) {
    if (!fp) return -1;
    return (int)lseek(fp->fd, off, whence);
}

long ftell(FILE *fp) {
    if (!fp) return -1;
    return lseek(fp->fd, 0, SEEK_CUR);
}

int feof(FILE *fp) {
    return fp ? fp->eof : 1;
}
