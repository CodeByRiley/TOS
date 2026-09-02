/* kernel/fs/stdio.c , kernel-internal FILE* over the VFS.
 *
 * Thin wrapper that maps fopen/fread/etc. onto the mounted filesystem. Used by
 * kernel callers (logger, ELF and PE loaders); userspace gets its own
 * implementation via syscalls.
 *
 * Mode strings follow C: mode[0] picks the base mode (r/w/a) and the rest
 * may only be '+' or 'b'. fread/fwrite return a count of complete items,
 * not bytes , the loaders read headers as `fread(&hdr, sizeof hdr, 1, fp)`
 * and test the result against 1.
 *
 * Persistence is a property of the mounted backend, not this layer.
 */
#include <fs/stdio.h>
#include <fs/vfs/vfs.h>
#include <memory/heap.h>
#include <utilities/log.h>

FILE *fopen(const char *name, const char *mode) {
    if (!name || !mode) return 0;

    int read = 0, write = 0, append = 0, truncate = 0, create = 0;

    /* Only mode[0] selects the base mode. Treating every character as a
     * flag makes "rw" , the common typo for "r+" , silently truncate a
     * file the caller asked to read.
     *
     * `create` tracks the base mode rather than `write`, because "r+"
     * opens for writing but must still fail on a missing file. */
    switch (mode[0]) {
        case 'r': read = 1; break;
        case 'w': write = 1; truncate = 1; create = 1; break;
        case 'a': write = 1; append = 1; create = 1; break;
        default: return 0;
    }

    for (int i = 1; mode[i] != '\0'; i++) {
        switch (mode[i]) {
            case '+': read = 1; write = 1; break;
            case 'b': break;                  /* Text and binary are equal. */
            default: return 0;
        }
    }

    FILE *fp = (FILE*)kmalloc(sizeof(FILE));
    if (!fp) return 0;

    int status = vfs_open(name, &fp->f);

    if (status == 0) {
        /* Existing file. Truncate in place rather than unlink+create: the
         * old sequence deleted the file before knowing whether a
         * replacement entry could be allocated, so a failure between the
         * two left the caller with nothing. */
        if (truncate)
            status = vfs_truncate(&fp->f);
        else if (append)
            status = vfs_seek(&fp->f, fp->f.size);
    } else if (create) {
        status = vfs_create(name, &fp->f);
    }

    if (status != 0) {
        vfs_close(&fp->f);
        kfree(fp);
        return 0;
    }

    fp->can_read = read;
    fp->can_write = write;
    fp->append = append;
    fp->valid = 1;

    return fp;
}

int fclose(FILE *fp) {
    if (!fp) return -1;
    /* Clear the in-use bit before freeing so a stale FILE* that survives
     * the kfree is rejected by the !valid checks instead of walking a
     * freed VFS handle. */
    fp->valid = 0;
    vfs_close(&fp->f);
    kfree(fp);
    return 0;
}

/* size * count without wrapping. An overflow would silently shrink the
 * request and report a short transfer that never happened. */
static int span_ok(usize size, usize count) {
    return size != 0 && count != 0 && count <= (usize)-1 / size;
}

usize fread(void *buf, usize size, usize count, FILE *fp) {
    if (!fp || !fp->valid || !fp->can_read) {
        return 0;
    }
    if (!span_ok(size, count)) return 0;

    /* Return the number of complete items read. */
    return vfs_read(&fp->f, buf, size * count) / size;
}

usize fwrite(const void *buf, usize size, usize count, FILE *fp) {
    if (!fp || !fp->valid || !fp->can_write) {
        return 0;
    }
    if (!span_ok(size, count)) return 0;

    /* Append means every write lands at EOF, not just the first one after
     * open , an intervening fseek must not move where data goes. */
    if (fp->append)
        vfs_seek(&fp->f, fp->f.size);

    /* Return the number of complete items written. */
    return vfs_write(&fp->f, buf, size * count) / size;
}

int fseek(FILE *fp, long off, int whence) {
    if (!fp || !fp->valid) return -1;
    u64 base;
    if      (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = fp->f.position;
    else if (whence == SEEK_END) base = fp->f.size;
    else return -1;
    u64 target;
    if (off < 0) {
        u64 distance = (u64)(-(off + 1)) + 1;
        if (distance > base) return -1;
        target = base - distance;
    } else {
        if ((u64)off > UINT64_MAX - base) return -1;
        target = base + (u64)off;
    }
    return vfs_seek(&fp->f, target);
}

long ftell(FILE *fp) {
    if (!fp || !fp->valid) return -1;
    return (long)fp->f.position;
}

int fgetc(FILE *fp) {
    if (!fp || !fp->valid || fp->f.position >= fp->f.size) return -1;
    u8 c;
    if (vfs_read(&fp->f, &c, 1) != 1) return -1;
    return c;
}

int feof(FILE *fp) {
    return !fp || !fp->valid || fp->f.position >= fp->f.size;
}
