/* kernel/fs/stdio.c — kernel-internal FILE* over FAT.
 *
 * Thin wrapper that maps fopen/fread/etc. onto the FAT driver. Used by
 * kernel callers (logger, ELF loader); userspace gets its own
 * implementation via syscalls. Mode strings are accepted but ignored —
 * the kernel-side handle is effectively read-only.
 */
#include "fs/stdio.h"
#include "fs/fat.h"
#include "memory/heap.h"

FILE *fopen(const char *name, const char *mode) {
    (void)mode;                       /* mode ignored — read-only path */
    FILE *fp = (FILE*)kmalloc(sizeof(FILE));
    if (!fp) return 0;
    if (fat_open(name, &fp->f) != 0) {
        kfree(fp);
        return 0;
    }
    fp->valid = 1;
    return fp;
}

int fclose(FILE *fp) {
    if (!fp) return -1;
    kfree(fp);
    return 0;
}

size_t fread(void *buf, size_t sz, size_t nmemb, FILE *fp) {
    if (!fp || !fp->valid) return 0;
    size_t n = fat_read(&fp->f, buf, sz * nmemb);
    return n / sz;
}

int fseek(FILE *fp, long off, int whence) {
    if (!fp || !fp->valid) return -1;
    uint32_t target;
    if      (whence == SEEK_SET) target = (uint32_t)off;
    else if (whence == SEEK_CUR) target = fp->f.pos + (uint32_t)off;
    else if (whence == SEEK_END) target = fp->f.size + (uint32_t)off;
    else return -1;
    return fat_seek(&fp->f, target);
}

long ftell(FILE *fp) {
    if (!fp || !fp->valid) return -1;
    return (long)fp->f.pos;
}

int fgetc(FILE *fp) {
    if (!fp || !fp->valid || fp->f.pos >= fp->f.size) return -1;
    uint8_t c;
    if (fat_read(&fp->f, &c, 1) != 1) return -1;
    return c;
}

int feof(FILE *fp) {
    return !fp || !fp->valid || fp->f.pos >= fp->f.size;
}
