/* userspace/lib/stat.c — POSIX stat()/fstat() over the kernel's compact
 * stat_user, plus mkdir().
 *
 * The kernel reports what FAT actually stores: size, directory flag, the
 * raw attribute byte, and the first cluster. Everything else in struct
 * stat is synthesised here:
 *
 *   - st_ino  = first cluster. Unique per file within one volume, which
 *               is what callers comparing inodes actually want, and 0 for
 *               the root and for empty files.
 *   - st_mode = type bits from the directory flag, permission bits from
 *               the FAT read-only attribute. There are no owners on FAT,
 *               so the same bits are reported for user, group, and other.
 *   - times   = 0. FAT stores a write timestamp but nothing has wired up
 *               a wall clock to convert it against, and a plausible-looking
 *               wrong time is worse than an obvious zero.
 */
#include "../include/sys/stat.h"
#include "syscall.h"

#define FAT_ATTR_READ_ONLY 0x01

static void fill_stat(const struct stat_user *k, struct stat *buf) {
    buf->st_dev   = 0;
    buf->st_ino   = k->first_cluster;
    buf->st_nlink = 1;
    buf->st_rdev  = 0;
    buf->st_size  = (off_t)k->size;
    buf->st_atime = 0;
    buf->st_mtime = 0;
    buf->st_ctime = 0;
    buf->st_uid   = 0;
    buf->st_gid   = 0;

    mode_t perm = (k->attr & FAT_ATTR_READ_ONLY)
                      ? (S_IRUSR | S_IRGRP | S_IROTH)
                      : (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH |
                         S_IWOTH);

    if (k->type == STAT_TYPE_DIR) {
        /* Directories need the execute bit or path traversal checks in
         * ported code reject them out of hand. */
        buf->st_mode = S_IFDIR | perm | S_IXUSR | S_IXGRP | S_IXOTH;
    } else {
        buf->st_mode = S_IFREG | perm;
    }
}

int stat(const char *path, struct stat *buf) {
    if (!path || !buf) return -1;

    struct stat_user k;
    if (stat_raw(path, &k) != 0) return -1;

    fill_stat(&k, buf);
    return 0;
}

int fstat(int fd, struct stat *buf) {
    if (!buf) return -1;

    struct stat_user k;
    if (fstat_raw(fd, &k) != 0) return -1;

    fill_stat(&k, buf);
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    (void)mode;   /* FAT has no permission bits to apply */
    return (int)mkdir_path(path);
}
